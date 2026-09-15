#include <pthread.h>
#include <iostream>
#include <string.h>
#include <mutex>
#include <liveMedia.hh>
#include <NetCommon.h>
#include <BasicUsageEnvironment.hh>
#include <UsageEnvironment.hh>
#include <errno.h>
#include <syslog.h>
#include <cvi_rtsp/rtsp.h>
#include "cvi_utils.hpp"
#include "cvi_video_smss.hpp"
#include "cvi_audio_smss.hpp"
#include "cvi_rtsp.hpp"

static std::mutex mutex;

int CVI_RTSP_Create(CVI_RTSP_CTX **ctx, CVI_RTSP_CONFIG *config)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (ctx == nullptr || config == nullptr) {
        SYSLOG(LOG_ERR, "nullptr ctx or config");
        return -1;
    }

    if (*ctx != nullptr) {
        return 0;
    }

    if (nullptr == (*ctx= new CVI_RTSP_CTX())) {
        SYSLOG(LOG_ERR, "malloc CVI_RTSP_CTX fail");
        return -1;
    }

    memset(*ctx, 0, sizeof(CVI_RTSP_CTX));

    CVI_TaskScheduler *scheduler = CVI_TaskScheduler::createNew();
    UsageEnvironment *env = BasicUsageEnvironment::createNew(*scheduler);

    UserAuthenticationDatabase *authDatabase = nullptr;
    if (config->authEnabled != 0) {
        if (config->username[0] == '\0' || config->password[0] == '\0') {
            SYSLOG(LOG_ERR, "RTSP Digest auth enabled but username/password is empty");
            env->reclaim();
            delete scheduler;
            delete (*ctx);
            *ctx = nullptr;
            return -1;
        }

        const char *realm = config->authRealm[0] != '\0'
                                ? config->authRealm
                                : "reCamera";
        authDatabase = new UserAuthenticationDatabase(realm, False);
        authDatabase->addUserRecord(config->username, config->password);
    }

    CVI_RTSP *server = CVI_RTSP::createNew(env, config, authDatabase);
    if (server == nullptr) {
        SYSLOG(LOG_ERR, "fail to create rtsp server");
        delete authDatabase;
        delete (*ctx);
        *ctx = nullptr;
        return -1;
    }

    (*ctx)->env = (void *)env;
    (*ctx)->server = (void *)server;
    (*ctx)->scheduler = (void *)scheduler;
    (*ctx)->authDatabase = (void *)authDatabase;

    memcpy(&((*ctx)->config), config, sizeof(CVI_RTSP_CONFIG));
    CVI_RTSP_ConfigureTcpSendRecovery(*ctx);

    return 0;
}

int CVI_RTSP_Destroy(CVI_RTSP_CTX **ctx)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (ctx == nullptr) {
        SYSLOG(LOG_ERR, "nullptr ctx");
        return -1;
    }

    CVI_RTSP_ClearTcpSendRecovery(*ctx);
    CVI_RTSP *server = static_cast<CVI_RTSP *>((*ctx)->server);
    UsageEnvironment *env = static_cast<UsageEnvironment *>((*ctx)->env);
    CVI_TaskScheduler *scheduler = static_cast<CVI_TaskScheduler *>((*ctx)->scheduler);
    UserAuthenticationDatabase *authDatabase =
        static_cast<UserAuthenticationDatabase *>((*ctx)->authDatabase);

    Medium::close(server);
    delete authDatabase;
    env->reclaim();
    delete scheduler;
    delete *ctx;

    *ctx = NULL;

    return 0;
}

void* rtspThread(void *arg)
{
    CVI_RTSP_CTX *ctx = static_cast<CVI_RTSP_CTX *>(arg);
    UsageEnvironment* env = static_cast<UsageEnvironment *>(ctx->env);

    env->taskScheduler().doEventLoop(&(ctx->stop)); // does not return

    return NULL;
}

int CVI_RTSP_Start(CVI_RTSP_CTX *ctx)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (ctx == nullptr) {
        SYSLOG(LOG_ERR, "nullptr ctx");
        return -1;
    }

    if (0 < ctx->worker) {
        return 0;
    }

    pthread_attr_t pthread_attr;
    struct sched_param sched_param;
    pthread_attr_init(&pthread_attr);
    pthread_attr_getschedparam(&pthread_attr, &sched_param);
    pthread_attr_setinheritsched(&pthread_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&pthread_attr, SCHED_RR);
    printf("prio:%d\n", sched_param.sched_priority);
    sched_param.sched_priority += 1;
    pthread_attr_setschedparam(&pthread_attr, &sched_param);
    if (0 > pthread_create(&(ctx->worker), &pthread_attr, rtspThread, (void *)ctx)) {
        SYSLOG(LOG_ERR, "rtsp worker create fail, %s", strerror(errno));
        return -1;
    }
    pthread_setname_np(ctx->worker, "rtsp_server");

    return 0;
}

int CVI_RTSP_Stop(CVI_RTSP_CTX *ctx)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (ctx == nullptr || ctx->server == nullptr) {
        SYSLOG(LOG_ERR, "nullptr ctx");
        return -1;
    }

    if (ctx->worker <= 0) {
        return 0;
    }

    ctx->stop = 1;

    if (0 > pthread_join(ctx->worker, NULL)) {
        SYSLOG(LOG_ERR, "fail to join rtsp worker, %s", strerror(errno));
        return -1;
    }
    ctx->worker = 0;
    ctx->stop = 0;

    return 0;
}

int CVI_RTSP_WriteFrame(CVI_RTSP_CTX *ctx, CVI_RTSP_TRACK track, CVI_RTSP_DATA *data)
{
    if (ctx == nullptr || track == nullptr || data == nullptr) {
        SYSLOG(LOG_ERR, "nullptr parameter");
        return -1;
    }

    if (CVI_RTSP_DATA_MAX_BLOCK < data->blockCnt) {
        SYSLOG(LOG_ERR, "block cnt(%d) exceeds limit(%d)", data->blockCnt, CVI_RTSP_DATA_MAX_BLOCK);
        return -1;
    }

    CVI_ServerMediaSubsession *smss = static_cast<CVI_ServerMediaSubsession *>(track);

    for (unsigned int i = 0; i < data->blockCnt; i++) {
        uint8_t* const block = data->dataPtr[i];
        const uint32_t length = data->dataLen[i];
        if (block == nullptr || length == 0) continue;

        // A VENC pack can contain an Annex-B access unit with several NALs
        // (notably SPS, PPS, and IDR).  H264VideoStreamDiscreteFramer expects
        // one NAL per input frame.  Passing the whole pack as one frame makes
        // Live555 advertise/send the SPS but lose the PPS, which leaves strict
        // clients such as Hikvision NVRs unable to decode the stream.
        uint32_t offset = 0;
        bool foundStartCode = false;
        while (offset + 3 < length) {
            uint32_t start = offset;
            while (start + 3 < length &&
                   !(block[start] == 0 && block[start + 1] == 0 &&
                     (block[start + 2] == 1 ||
                      (start + 3 < length && block[start + 2] == 0 && block[start + 3] == 1)))) {
                ++start;
            }
            if (start + 3 >= length) break;
            foundStartCode = true;
            const uint32_t prefix = block[start + 2] == 1 ? 3 : 4;
            const uint32_t nalBegin = start + prefix;
            uint32_t next = nalBegin;
            while (next + 3 < length &&
                   !(block[next] == 0 && block[next + 1] == 0 &&
                     (block[next + 2] == 1 ||
                      (next + 3 < length && block[next + 2] == 0 && block[next + 3] == 1)))) {
                ++next;
            }
            const uint32_t nalEnd = next + 3 < length ? next : length;
            if (nalEnd > nalBegin) smss->writeData(block + nalBegin, nalEnd - nalBegin);
            if (next + 3 >= length) break;
            offset = next;
        }
        if (!foundStartCode) smss->writeData(block, length);
    }

    return 0;
}

int CVI_RTSP_CreateSession(CVI_RTSP_CTX *ctx, CVI_RTSP_SESSION_ATTR *attr, CVI_RTSP_SESSION **session)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (ctx == nullptr || attr == nullptr) {
        SYSLOG(LOG_ERR, "null parameter");
        return -1;
    }

    if (attr->video.codec == RTSP_VIDEO_NONE && attr->audio.codec == RTSP_AUDIO_NONE) {
        SYSLOG(LOG_ERR, "no video codec and audio codec");
        return -1;
    }

	if (attr->audio.codec != RTSP_AUDIO_NONE && attr->audio.sampleRate == 0) {
		SYSLOG(LOG_ERR, "no audio sample rate");
		return -1;
	}

    if (nullptr == (*session = (CVI_RTSP_SESSION *)malloc(sizeof(CVI_RTSP_SESSION)))) {
        SYSLOG(LOG_ERR, "malloc session fail");
        return -1;
    }

    memset(*session, 0, sizeof(CVI_RTSP_SESSION));

    UsageEnvironment *env = static_cast<UsageEnvironment *>(ctx->env);
    CVI_ServerMediaSession* sms = CVI_ServerMediaSession::createNew(*env, attr->name, attr->name, "");
    if (attr->maxConnNum > 0) {
        sms->setMaxConn(attr->maxConnNum);
    }

    CVI_RTSP *server = static_cast<CVI_RTSP *>(ctx->server);

    if (attr->video.codec != RTSP_VIDEO_NONE) {
        CVI_VideoSubsession *video = CVI_VideoSubsession::createNew(*env, *attr);

        sms->addSubsession(video);

        (*session)->video= (void *)video;
    }

    if (attr->audio.codec != RTSP_AUDIO_NONE) {
        CVI_AudioSubsession *audio = CVI_AudioSubsession::createNew(*env, *attr);

        sms->addSubsession(audio);

        (*session)->audio= (void *)audio;
    }

    if (ctx->config.packetLen != 0) {
        CVI_ServerMediaSubsession::packetLen = ctx->config.packetLen;
    }

    server->addServerMediaSession(sms);
    snprintf((*session)->name, sizeof((*session)->name), "%s", attr->name);

    char* url = server->rtspURL(sms);
    std::cout << url << std::endl;
    delete[] url;

    return 0;
}

int CVI_RTSP_DestroySession(CVI_RTSP_CTX *ctx, CVI_RTSP_SESSION *session)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (ctx == nullptr || session == nullptr) {
        SYSLOG(LOG_ERR, "nullptr parameter");
        return -1;
    }

    CVI_RTSP *server = static_cast<CVI_RTSP *>(ctx->server);
    server->removeServerMediaSession(session->name);

    free(session);

    return 0;
}

int CVI_RTSP_SetListener(CVI_RTSP_CTX *ctx, CVI_RTSP_STATE_LISTENER *listener)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (ctx == nullptr) {
        SYSLOG(LOG_ERR, "nullptr ctx");
        return -1;
    }

    CVI_RTSP *server = static_cast<CVI_RTSP *>(ctx->server);
    server->setListener(listener->onConnect, listener->argConn, listener->onDisconnect, listener->argDisconn);

    return 0;
}
