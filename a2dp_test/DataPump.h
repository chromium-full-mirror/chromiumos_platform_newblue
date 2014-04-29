/*
 * Copyright (C) 2013 The Android Open Source Project
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

#ifndef DATA_PUMP_H
#define DATA_PUMP_H

#include <pthread.h>

class DataPump {
public:
    DataPump(size_t bufferSize);
    virtual ~DataPump();

    void start();
    void stop();
    void wait();

    void* getEmptyBuffer();
    void setBufferCount(void* buffer, size_t count);

    inline size_t getBufferSize() const { return mBufferSize; }

private:
    void readLoop();
    void writeLoop();

    void* getFullBuffer(size_t& count);

    static void* readThread(void* arg);
    static void* writeThread(void* arg);

protected:
    virtual int write(void* buffer, int size) = 0;

private:
    void*           mBuffer1;
    void*           mBuffer2;
    size_t          mBuffer1Count;
    size_t          mBuffer2Count;
    int             mNextWriteBuffer;
    size_t          mBufferSize;
    pthread_t       mReadThread;
    pthread_t       mWriteThread;
    bool            mDone;
    pthread_cond_t  mCondition;
    pthread_mutex_t mMutex;

};

#endif // DATA_PUMP_H
