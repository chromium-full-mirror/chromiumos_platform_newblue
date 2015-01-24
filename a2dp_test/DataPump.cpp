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

#include "DataPump.h"

#include <stdio.h>
#include <stdlib.h>

DataPump::DataPump(size_t bufferSize)
    :   mBuffer1(NULL),
        mBuffer2(NULL),
        mBuffer1Count(0),
        mBuffer2Count(0),
        mNextWriteBuffer(0),
        mBufferSize(bufferSize),
        mDone(false)
{
    mBuffer1 = malloc(bufferSize);
    mBuffer2 = malloc(bufferSize);
    pthread_cond_init(&mCondition, NULL);
    pthread_mutex_init(&mMutex, NULL);
}
    
DataPump::~DataPump() {
    free(mBuffer1);
    free(mBuffer2);
}

void DataPump::start() {
    mBuffer1 = realloc(mBuffer1, mBufferSize);
    mBuffer2 = realloc(mBuffer2, mBufferSize);
    mBuffer1Count = 0;
    mBuffer2Count = 0;
    mNextWriteBuffer = 0;
    mDone = false;
    pthread_create(&mWriteThread, NULL, writeThread, this);
}

void DataPump::stop() {
    pthread_mutex_lock(&mMutex);
    mDone = true;
    pthread_cond_broadcast(&mCondition);    
    pthread_mutex_unlock(&mMutex);
}

void DataPump::wait() {
    pthread_join(mReadThread, NULL);
    pthread_join(mWriteThread, NULL);

}

void DataPump::writeLoop() {
    while (1) {
        size_t count;
        void* buffer = getFullBuffer(count);
        if (!buffer) break;
        for (size_t offset = 0; offset < count; ) {
            int written = write((char *)buffer + offset, count - offset);
            if (written <= 0) break;
            offset += written;
        }
        setBufferCount(buffer, 0);
    }
printf("writeLoop done\n");
}

void* DataPump::getEmptyBuffer() {
    void* result = NULL;

    pthread_mutex_lock(&mMutex);
    while (mBuffer1Count && mBuffer2Count && !mDone) {
        pthread_cond_wait(&mCondition, &mMutex);
    }
    if (mBuffer1Count == 0) {
        result = mBuffer1;
    } else if (mBuffer2Count == 0) {
        result = mBuffer2;
    }
    // else we are done
    pthread_mutex_unlock(&mMutex);
    return result;
}

void* DataPump::getFullBuffer(size_t& count) {
    void* result = NULL;

    pthread_mutex_lock(&mMutex);
    while (!mNextWriteBuffer && !mDone) {
        pthread_cond_wait(&mCondition, &mMutex);
    }
    if (mNextWriteBuffer == 1 && mBuffer1Count > 0) {
        result = mBuffer1;
        count = mBuffer1Count;
        mNextWriteBuffer = (mBuffer2Count ? 2 : 0);
    } else if (mNextWriteBuffer == 2 && mBuffer2Count > 0) {
        result = mBuffer2;
        count = mBuffer2Count;
        mNextWriteBuffer = (mBuffer1Count ? 1 : 0);
    }
    // else we are done
    pthread_mutex_unlock(&mMutex);
    return result;
}
 
void DataPump::setBufferCount(void* buffer, size_t count) {
    pthread_mutex_lock(&mMutex);
    if (buffer == mBuffer1) {
        mBuffer1Count = count;
        if (count && !mNextWriteBuffer) {
            mNextWriteBuffer = 1;
        }
    } else if (buffer == mBuffer2) {
        mBuffer2Count = count;
        if (count && !mNextWriteBuffer) {
            mNextWriteBuffer = 2;
        }
    }
    pthread_cond_broadcast(&mCondition);
    pthread_mutex_unlock(&mMutex);
}


void* DataPump::writeThread(void* arg) {
    ((DataPump *)arg)->writeLoop();
    return NULL;
}
