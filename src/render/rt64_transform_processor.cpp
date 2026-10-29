//
// RT64
//

#include "rt64_transform_processor.h"

#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/rt64_math.h"
#include "common/rt64_wcw_logic.h"
#include "hle/rt64_game_frame.h"
#include "hle/rt64_workload_queue.h"

namespace RT64 {
    // [wcw fix] aki-roadmap#110: the matrix a generated frame in a match draws an unmapped transform with. Its current
    // matrix, unless its vertices cross the camera's near plane (WcwLogic::pinUnmappedToRealView): then its current
    // matrix moved from the interpolated view-projection to the real one (world * real * inverse(interpolated)), so it
    // clips as in the real frame. Needs every vertex on the same view-projection; returns the current matrix otherwise.
    // The pinned matrix isn't affine (it carries half a game frame of camera motion); lighting, texgen and RT treat
    // it as the object's matrix, which is off by that much for these few near-plane draws.
    static hlslpp::float4x4 wcwUnmappedMatrix(const DrawData &drawData, uint32_t t, bool inMatch, bool generated, std::vector<float> &realClip, std::vector<float> &genClip) {
        const hlslpp::float4x4 world(drawData.worldTransforms[t]);
        if (!inMatch || !generated || (t >= drawData.worldTransformVertexIndices.size())) {
            return world;
        }

        const uint32_t first = drawData.worldTransformVertexIndices[t];
        const uint32_t count = drawData.worldTransformVertexCount(t);
        if ((count == 0) || ((size_t(first) + count) > drawData.viewProjIndices.size()) || (((size_t(first) + count) * 3) > drawData.posFloats.size())) {
            return world;
        }

        const uint16_t vp = drawData.viewProjIndices[first];
        if ((vp >= drawData.viewProjTransforms.size()) || (vp >= drawData.modViewProjTransforms.size()) || (vp >= drawData.projRatioScales.size())) {
            return world;
        }

        interop::float4x4 realDrawn;
        WcwLogic::drawnViewProj(&drawData.viewProjTransforms[vp].m[0][0], drawData.projRatioScales[vp], &realDrawn.m[0][0]);
        if (memcmp(&realDrawn, &drawData.modViewProjTransforms[vp], sizeof(interop::float4x4)) == 0) {
            return world;
        }

        const hlslpp::float4x4 realViewProj(realDrawn);
        const hlslpp::float4x4 genViewProj(drawData.modViewProjTransforms[vp]);
        const hlslpp::float4x4 realMatrix = hlslpp::mul(world, realViewProj);
        const hlslpp::float4x4 genMatrix = hlslpp::mul(world, genViewProj);
        realClip.resize(size_t(count) * 4);
        genClip.resize(size_t(count) * 4);
        uint32_t used = 0;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t v = first + i;
            if (drawData.viewProjIndices[v] != vp) {
                return world;
            }

            // Vertices copied for modifies can land in this range with another transform's index; leave them out.
            if ((v < drawData.worldIndices.size()) && (drawData.worldIndices[v] != t)) {
                continue;
            }

            const hlslpp::float4 pos(drawData.posFloats[v * 3 + 0], drawData.posFloats[v * 3 + 1], drawData.posFloats[v * 3 + 2], 1.0f);
            hlslpp::store(hlslpp::mul(pos, realMatrix), &realClip[used * 4]);
            hlslpp::store(hlslpp::mul(pos, genMatrix), &genClip[used * 4]);
            used++;
        }

        if (!WcwLogic::pinUnmappedToRealView(inMatch, generated, WcwLogic::crossesNearPlane(realClip.data(), used), WcwLogic::crossesNearPlane(genClip.data(), used))) {
            return world;
        }

        const hlslpp::float4x4 pinned = hlslpp::mul(realMatrix, hlslpp::inverse(genViewProj));
        float values[16];
        hlslpp::store(pinned, values);
        for (int k = 0; k < 16; k++) {
            if (!std::isfinite(values[k])) {
                return world;
            }
        }

        // WCW_PIN_LOG=1: how often this fires (stderr, a running count at most once a second).
        static const bool pinLog = getenv("WCW_PIN_LOG") != nullptr;
        if (pinLog) {
            static uint64_t pins = 0;
            static std::chrono::steady_clock::time_point lastPrint;
            pins++;
            const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
            if ((now - lastPrint) >= std::chrono::seconds(1)) {
                fprintf(stderr, "[wcw pin] %llu unmapped near-plane transforms drawn with the real view so far\n", (unsigned long long)(pins));
                lastPrint = now;
            }
        }

        return pinned;
    }

    // TransformProcessor
    
    TransformProcessor::TransformProcessor() { }

    TransformProcessor::~TransformProcessor() { }

    void TransformProcessor::setup(RenderWorker *worker) {
        bufferUploader = std::make_unique<BufferUploader>(worker->device);
    }

    void TransformProcessor::process(const ProcessParams &p) {
        for (uint32_t w : p.curFrame->workloads) {
            Workload &workload = p.workloadQueue->workloads[w];
            DrawData &drawData = workload.drawData;
            const bool prevFrameValid = (p.prevFrame != nullptr) && p.curFrame->frameMap.workloads[w].mapped;
            auto &lerpWorldTransforms = drawData.lerpWorldTransforms;
            auto &invTWorldTransforms = drawData.invTWorldTransforms;
            auto &prevWorldTransforms = drawData.prevWorldTransforms;
            lerpWorldTransforms.clear();
            invTWorldTransforms.clear();
            prevWorldTransforms.clear();

            hlslpp::float4x4 prevMatrix, curMatrix, invMatrix, invTMatrix;

            // Match with the previous frame and interpolate the transforms.
            if (prevFrameValid) {
                const GameFrameMap::WorkloadMap &workloadMap = p.curFrame->frameMap.workloads[w];
                const DrawData &prevDrawData = p.workloadQueue->workloads[workloadMap.prevWorkloadIndex].drawData;
                const bool inMatch = wcwInterpActive.load(std::memory_order_relaxed);
                // The debugger camera replaces the interpolated view; don't pin draws back to the game's.
                const bool generated = WcwLogic::generatedFrame(p.curFrameWeight) && !workload.debuggerCamera.enabled;
                for (size_t t = 0; t < drawData.worldTransforms.size(); t++) {
                    const GameFrameMap::TransformMap &transformMap = workloadMap.transforms[t];
                    if (transformMap.mapped) {
                        // [wcw fix] Transforms paired from their camera-independent matrices (usedPureMatrix)
                        // interpolate those, then get the camera put back below.
                        const bool usedPure = transformMap.usedPureMatrix;
                        const hlslpp::float4x4 &prevTransform = usedPure
                            ? prevDrawData.worldTransformsPure[transformMap.prevTransformIndex]
                            : prevDrawData.worldTransforms[transformMap.prevTransformIndex];
                        const hlslpp::float4x4 &curTransform = usedPure ? drawData.worldTransformsPure[t] : drawData.worldTransforms[t];

                        prevMatrix = transformMap.rigidBody.lerp(p.prevFrameWeight, prevTransform, curTransform, true);
                        curMatrix = transformMap.rigidBody.lerp(p.curFrameWeight, prevTransform, curTransform, true);

                        if (usedPure) {
                            // [wcw fix] Put the camera back. In a match the camera is interpolated too
                            // (componentwise, per transform), so interpolated objects and the scene around
                            // them are viewed from the same in-between camera during pans.
                            const hlslpp::float4x4 &camera = drawData.worldTransformsCamera[t];
                            const uint32_t prevIdx = transformMap.prevTransformIndex;
                            if (inMatch && (prevIdx < prevDrawData.worldTransformsCamera.size())) {
                                const hlslpp::float4x4 prevCamera(prevDrawData.worldTransformsCamera[prevIdx]);
                                auto lerpCamera = [](const hlslpp::float4x4 &x, const hlslpp::float4x4 &y, float weight) {
                                    hlslpp::float4x4 r;
                                    for (int i = 0; i < 4; i++) {
                                        r[i] = hlslpp::lerp(x[i], y[i], hlslpp::float4(weight));
                                    }
                                    return r;
                                };

                                prevMatrix = hlslpp::mul(prevMatrix, lerpCamera(prevCamera, camera, p.prevFrameWeight));
                                curMatrix = hlslpp::mul(curMatrix, lerpCamera(prevCamera, camera, p.curFrameWeight));
                            }
                            else {
                                prevMatrix = hlslpp::mul(prevMatrix, camera);
                                curMatrix = hlslpp::mul(curMatrix, camera);
                            }

                            // [wcw fix] pure * camera is the raw uploaded MVP, but unmatched draws render with
                            // worldTransforms[t] (MVP * inv(viewProj) * extended viewProj). Apply the same
                            // correction K = inv(pure * camera) * worldTransforms[t] so both kinds of draw are in
                            // the same space; at weight 1.0 this gives exactly worldTransforms[t].
                            const hlslpp::float4x4 mvpRef = hlslpp::mul(drawData.worldTransformsPure[t], camera);
                            const hlslpp::float4x4 kMat = hlslpp::mul(hlslpp::inverse(mvpRef), drawData.worldTransforms[t]);
                            const hlslpp::float4x4 curFixed = hlslpp::mul(curMatrix, kMat);
                            const hlslpp::float4x4 prevFixed = hlslpp::mul(prevMatrix, kMat);
                            float ck[16], pk[16];
                            hlslpp::store(curFixed, ck);
                            hlslpp::store(prevFixed, pk);
                            bool finite = true;
                            for (int k = 0; k < 16; k++) {
                                if (!std::isfinite(ck[k]) || !std::isfinite(pk[k])) {
                                    finite = false;
                                    break;
                                }
                            }

                            if (finite) {
                                curMatrix = curFixed;
                                prevMatrix = prevFixed;
                            }
                        }

                        // [wcw fix] Pairs flagged by GameFrame::matchTransform (implausible swing of the
                        // camera-independent pair) blend the drawn matrices componentwise instead.
                        if (transformMap.wcwCombinedLerp) {
                            const uint32_t prevIdx = transformMap.prevTransformIndex;
                            if (prevIdx < prevDrawData.worldTransforms.size()) {
                                const hlslpp::float4x4 prevWorld(prevDrawData.worldTransforms[prevIdx]);
                                const hlslpp::float4x4 curWorld(drawData.worldTransforms[t]);
                                for (int i = 0; i < 4; i++) {
                                    prevMatrix[i] = hlslpp::lerp(prevWorld[i], curWorld[i], hlslpp::float4(p.prevFrameWeight));
                                    curMatrix[i] = hlslpp::lerp(prevWorld[i], curWorld[i], hlslpp::float4(p.curFrameWeight));
                                }
                            }
                        }

                        // [wcw fix] In a match, an interpolated matrix whose magnitude differs from the real
                        // current matrix by more than 2x is wrong (unstable decompositions, e.g. edge-on
                        // billboards at low camera angles): render that draw at the current frame instead.
                        if (inMatch) {
                            auto matrixNorm = [](const hlslpp::float4x4 &m) {
                                float acc = 0.0f;
                                for (int i = 0; i < 3; i++) {
                                    acc += float(hlslpp::dot(m[i].xyz, m[i].xyz));
                                }
                                return sqrtf(acc);
                            };

                            const float refNorm = matrixNorm(drawData.worldTransforms[t]);
                            const float lerpNorm = matrixNorm(curMatrix);
                            const bool degenerate = !std::isfinite(lerpNorm) || (lerpNorm < 1e-6f) || (refNorm < 1e-6f);
                            const float ratio = degenerate ? FLT_MAX : ((lerpNorm > refNorm) ? (lerpNorm / refNorm) : (refNorm / lerpNorm));
                            if (degenerate || (ratio > 2.0f)) {
                                curMatrix = drawData.worldTransforms[t];
                                prevMatrix = drawData.worldTransforms[t];
                            }
                        }

                        invMatrix = hlslpp::inverse(curMatrix);
                        invTMatrix = hlslpp::transpose(invMatrix);
                        lerpWorldTransforms.emplace_back(curMatrix);
                        invTWorldTransforms.emplace_back(invTMatrix);
                        prevWorldTransforms.emplace_back(prevMatrix);
                    }
                    else {
                        const hlslpp::float4x4 unmappedMatrix = wcwUnmappedMatrix(drawData, uint32_t(t), inMatch, generated, wcwRealClip, wcwGenClip);
                        invMatrix = hlslpp::inverse(unmappedMatrix);
                        invTMatrix = hlslpp::transpose(invMatrix);
                        lerpWorldTransforms.emplace_back(unmappedMatrix);
                        invTWorldTransforms.emplace_back(invTMatrix);
                        prevWorldTransforms.emplace_back(unmappedMatrix);
                    }
                }
            }
            // Copy as normal and just generate the inverse of the transforms.
            else {
                for (size_t t = 0; t < drawData.worldTransforms.size(); t++) {
                    invMatrix = hlslpp::inverse(drawData.worldTransforms[t]);
                    invTMatrix = hlslpp::transpose(invMatrix);
                    invTWorldTransforms.push_back(invTMatrix);
                }
            }
        }
    }
    
    void TransformProcessor::upload(const ProcessParams &p) {
        uploads.clear();

        for (uint32_t w : p.curFrame->workloads) {
            const bool prevFrameValid = (p.prevFrame != nullptr);
            Workload &workload = p.workloadQueue->workloads[w];
            const DrawData &drawData = workload.drawData;
            DrawBuffers &drawBuffers = workload.drawBuffers;
            const interop::float4x4 *worldMatrices = prevFrameValid ? drawData.lerpWorldTransforms.data() : drawData.worldTransforms.data();
            const interop::float4x4 *prevWorldMatrices = prevFrameValid ? drawData.prevWorldTransforms.data() : drawData.worldTransforms.data();
            const interop::float4x4 *invTWorldMatrices = drawData.invTWorldTransforms.data();
            std::pair<size_t, size_t> uploadRange = { 0, drawData.worldTransforms.size() };
            uploads.emplace_back(BufferUploader::Upload{ worldMatrices, uploadRange, sizeof(interop::float4x4), RenderBufferFlag::STORAGE, { }, &drawBuffers.worldTransformsBuffer });
            uploads.emplace_back(BufferUploader::Upload{ prevWorldMatrices, uploadRange, sizeof(interop::float4x4), RenderBufferFlag::STORAGE, { }, &drawBuffers.prevWorldTransformsBuffer });
            uploads.emplace_back(BufferUploader::Upload{ invTWorldMatrices, uploadRange, sizeof(interop::float4x4), RenderBufferFlag::STORAGE, { }, &drawBuffers.invTWorldTransformsBuffer });
        }

        bufferUploader->submit(p.worker, uploads);
    }
};