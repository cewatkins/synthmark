/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SYNTHMARK_LATENCYMARK_HARNESS_H
#define SYNTHMARK_LATENCYMARK_HARNESS_H

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <sstream>

#include "AudioSinkBase.h"
#include "SynthMark.h"
#include "synth/Synthesizer.h"
#include "tools/LogTool.h"
#include "tools/TestHarnessBase.h"
#include "tools/TimingAnalyzer.h"
#include "TestHarnessParameters.h"


#define NOTES_PER_STEP   10

enum VoicesMode {
    VOICES_UNDEFINED,
    VOICES_SWITCH,
    VOICES_RANDOM,
    VOICES_LINEAR_LOOP,
};

/**
 * Determine buffer latency required to avoid glitches.
 * The "LatencyMark" is the minimum buffer size that is a multiple
 * of a burst size that can be used for N minutes without glitching.
 */
class LatencyMarkHarness : public TestHarnessBase {
public:
    LatencyMarkHarness(AudioSinkBase *audioSink,
                       SynthMarkResult *result,
                       LogTool *logTool = nullptr)
    : TestHarnessBase(audioSink, result, logTool)
    {
        mTestName = "LatencyMark";
        TestHarnessParameters::setNumVoices(kSynthmarkNumVoicesLatency);

        // Constant seed to obtain a fixed pattern of pseudo-random voices
        // for experiment repeatability and reproducibility
        //srand(time(NULL)); // "Random" seed
        srand(0); // Fixed seed
    }

    virtual ~LatencyMarkHarness() {
    }

    // Print notices of glitches and restarts in another thread
    // so that the printing will not cause a glitch.
    void startMonitorCallback() {
        mMonitorEnabled = true;
        mMonitorThread = new HostThread();
        int err = mMonitorThread->start(threadProcWrapper, this);
        if (err != 0) {
            delete mMonitorThread;
            mMonitorThread = nullptr;
        }
    }

    void stopMonitorCallback() {
        if (mMonitorThread != nullptr) {
            mMonitorEnabled = false;
            mMonitorThread->join();
            delete mMonitorThread;
            mMonitorThread = nullptr;
        }
    }

    //
    void monitorCallback() {
        static const int kMonitorPeriodMicros = 80000;
        int32_t previousBufferSize = mAudioSink->getBufferSizeInFrames();
        while (mMonitorEnabled) {
            HostTools::sleepForNanoseconds(kMonitorPeriodMicros * SYNTHMARK_NANOS_PER_MICROSECOND);

            int32_t currentBufferSize = mAudioSink->getBufferSizeInFrames();
            if (currentBufferSize != previousBufferSize) {
                printf("Audio glitch at %.2fs, "
                               "restarting test with buffer size %d = %d * %d\n",
                       mGlitchTime, currentBufferSize,
                       currentBufferSize / mFramesPerBurst, mFramesPerBurst);
                fflush (stdout);
                previousBufferSize = currentBufferSize;
            }
        }
    }

    static void * threadProcWrapper(void *arg) {
        LatencyMarkHarness *harness = (LatencyMarkHarness *) arg;
        harness->monitorCallback();
        return NULL;
    }

// Run the benchmark.
    int32_t runTest(int32_t sampleRate, int32_t framesPerBurst, int32_t numSeconds) override {
        startMonitorCallback();
        int32_t result = TestHarnessBase::runTest(sampleRate, framesPerBurst, numSeconds);
        stopMonitorCallback();
        return result;
    }

    virtual void onBeginMeasurement() override {
        mPreviousUnderrunCount = 0;
        mAudioSink->setBufferSizeInFrames(mFramesPerBurst);
        mLogTool->log("---- Measure latency ---- #voices = %d / %d\n",
                      getNumVoices(),
                      getNumVoicesHigh());

        setupJitterRecording();
    }

    void restart() {
        // Reset clock so we get a full run without glitches.
        mFrameCounter = 0;
        mNoteCounter = 0;
    }

    virtual int32_t onBeforeNoteOn() override {
        if (mTimer.getActiveTime() > 0) {
            int32_t underruns = mAudioSink->getUnderrunCount();
            if (underruns > mPreviousUnderrunCount) {
                mPreviousUnderrunCount = underruns;
                // Increase latency to avoid glitches.
                int32_t sizeInFrames = mAudioSink->getBufferSizeInFrames();
                int32_t desiredSizeInFrames = sizeInFrames + mFramesPerBurst;
                int32_t actualSize = mAudioSink->setBufferSizeInFrames(desiredSizeInFrames);
                if (actualSize < desiredSizeInFrames) {
                    mLogTool->log("ERROR - at maximum buffer size and still glitching\n");
                    return SYNTHMARK_RESULT_UNRECOVERABLE_ERROR;
                }
                // Record when the glitch occurred.
                mGlitchTime = ((float)mFrameCounter / mSampleRate);

                if (isVerbose()) {
                    printf("%s() detected glitch at %5.2f\n", __func__, mGlitchTime);
                    fflush(stdout);
                }

                restart();
            }
        }
        return SYNTHMARK_RESULT_SUCCESS;
    }

    /**
     * calculate the final size in frames of the output buffer
     */
    virtual void onEndMeasurement() override {
        int32_t sizeFrames = mAudioSink->getBufferSizeInFrames();
        double latencyMsec = 1000.0 * sizeFrames / mSampleRate;

        std::stringstream resultMessage;
        resultMessage << dumpJitter();
        resultMessage << "frames.per.burst     = " << mFramesPerBurst << std::endl;
        resultMessage << "audio.latency.bursts = " << (sizeFrames / mFramesPerBurst) << std::endl;
        resultMessage << "audio.latency.frames = " << sizeFrames << std::endl;
        resultMessage << "audio.latency.msec   = " << latencyMsec << std::endl;

        resultMessage << mCpuAnalyzer.dump();

        mResult->appendMessage(resultMessage.str());
        mResult->setResultCode(SYNTHMARK_RESULT_SUCCESS);
        mResult->setMeasurement((double) sizeFrames);
    }

    void setVoicesMode(VoicesMode vm) {
        mVoicesMode = vm;
    }

    int32_t getCurrentNumVoices() override {
        if (mNumVoicesHigh > 0) {
            // The same number of voices is kept every (NOTES_PER_STEP / 2).
            bool needUpdate = ((getNoteCounter() % (NOTES_PER_STEP / 2)) == 0);
            static int32_t lastVoices;

            if (needUpdate) {
                switch (mVoicesMode) {
                    case VOICES_LINEAR_LOOP:
                        // The number of voices is linearly incremented in the
                        // range [-n, -N]. When it reaches -N, it restarts back
                        // from -n.
                        lastVoices += NOTES_PER_STEP / 2;
                        if (lastVoices > mNumVoicesHigh ||
                                lastVoices < getNumVoices())
                            lastVoices = getNumVoices();
                        break;
                    case VOICES_RANDOM:
                        // Return a number of voices in the range [-n, -N].
                        lastVoices = (rand() % (mNumVoicesHigh - getNumVoices() + 1))
                                + getNumVoices();
                        break;
                    case VOICES_SWITCH:
                    default:
                        // Start low then high then low and so on.
                        // Pattern should restart on each test.
                        lastVoices = ((getNoteCounter() % NOTES_PER_STEP) < (NOTES_PER_STEP / 2))
                                     ? getNumVoices()
                                     : mNumVoicesHigh;
                        break;
                }
            }
            if (isVerbose()) {
                printf("%s() returns %d\n", __func__, lastVoices);
                fflush(stdout);
            }
            return lastVoices;
        } else {
            return getNumVoices();
        }
    }

private:

    int32_t           mPreviousUnderrunCount = 0;
    VoicesMode        mVoicesMode = VOICES_SWITCH;

    HostThread       *mMonitorThread = nullptr;
    bool              mMonitorEnabled = true; // atomic is not sufficiently portable
    float             mGlitchTime = 0.0f;
};

#endif // SYNTHMARK_LATENCYMARK_HARNESS_H
