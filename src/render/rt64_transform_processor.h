//
// RT64
//

#pragma once

#include "rt64_buffer_uploader.h"

namespace RT64 {
    struct GameFrame;
    struct WorkloadQueue;

    struct TransformProcessor {
        std::unique_ptr<BufferUploader> bufferUploader;
        std::vector<BufferUploader::Upload> uploads;
        std::vector<float> wcwRealClip; // [wcw fix] scratch for unmapped transforms (aki-roadmap#110)
        std::vector<float> wcwGenClip;

        struct ProcessParams {
            RenderWorker *worker = nullptr;
            WorkloadQueue *workloadQueue = nullptr;
            GameFrame *curFrame = nullptr;
            const GameFrame *prevFrame = nullptr;
            float curFrameWeight = 1.0f;
            float prevFrameWeight = 0.0f;
        };

        TransformProcessor();
        ~TransformProcessor();
        void setup(RenderWorker *worker);
        void process(const ProcessParams &p);
        void upload(const ProcessParams &p);
    };
};