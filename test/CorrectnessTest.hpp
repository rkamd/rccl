/*************************************************************************
 * Copyright (c) 2019-2021 Advanced Micro Devices, Inc. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/
#ifndef CORRECTNESSTEST_HPP
#define CORRECTNESSTEST_HPP

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <tuple>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>
#include <math.h>

#include "rccl.h"
#include "../include/rccl_bfloat16.h"

#include "TestChecks.hpp"

#define MAX_ENV_TOKENS 16

namespace CorrectnessTests
{
    typedef enum { ncclCollBroadcast, ncclCollReduce, ncclCollAllGather, ncclCollReduceScatter, ncclCollAllReduce, ncclCollGather, ncclCollScatter, ncclCollAllToAll, ncclCollSendRecv } ncclFunc_t;
    typedef enum { ncclInputBuffer, ncclOutputBuffer } ncclBufferType_t;

    // Performs the various basic reduction operations
    template <typename T>
    T ReduceOp(ncclRedOp_t const op, T const A, T const B)
    {
        switch (op)
        {
        case ncclSum:  return A + B;
        case ncclProd: return A * B;
        case ncclMax:  return std::max(A, B);
        case ncclMin:  return std::min(A, B);
        default:
            fprintf(stderr, "[ERROR] Unsupported reduction operator (%d)\n", op);
            exit(0);
        }
    }

    // Returns the number of bytes per element for each supported datatype
    static int DataTypeToBytes(ncclDataType_t const dataType)
    {
        switch (dataType)
        {
        case ncclInt8:   return 1;
        case ncclUint8:  return 1;
        case ncclInt32:  return 4;
        case ncclUint32: return 4;
        case ncclInt64:  return 8;
        case ncclUint64: return 8;
        case ncclFloat16: return 2;
        case ncclFloat32: return 4;
        case ncclFloat64: return 8;
        case ncclBfloat16: return 2;
        default:
            fprintf(stderr, "[ERROR] Unsupported datatype (%d)\n", dataType);
            exit(0);
        }
    }

    // Encapsulates all the memory used per devices for collectives, as well as reference results
    struct Dataset
    {
        int                 numDevices;  // Number of devices participating
        size_t              numElements; // Number of elements per array
        ncclDataType_t      dataType;    // Data type of each input/output pointer
        bool                inPlace;     // Whether or not output pointers are same as input pointers
        ncclFunc_t          function;    // Buffer sizes are different in case of gather, scatter and all to all

        std::vector<void *> inputs;      // Input pointers (1 per device)
        std::vector<void *> outputs;     // Output pointers (1 per device)
                                         // May be identical to input pointers for in-place tests
        std::vector<void *> expected;    // Expected output (1 per device)

        size_t NumBytes() const
        {
            return numElements * DataTypeToBytes(dataType);
        }

        size_t NumBytes(ncclBufferType_t bufferType) const
        {
            if ((function == ncclCollGather && (bufferType == ncclOutputBuffer || inPlace == true)) ||
                (function == ncclCollScatter && bufferType == ncclInputBuffer) ||
                function == ncclCollAllToAll)
                return numElements * DataTypeToBytes(dataType) * numDevices;
            return numElements * DataTypeToBytes(dataType);
        }

        // Checks if the current HIP Runtime and GPU support managed memory
        bool SupportsHmm()
        {
            hipDeviceProp_t device_prop;
            int device_id;
            hipGetDevice(&device_id);
            hipGetDeviceProperties(&device_prop, device_id);
            if (device_prop.managedMemory == 1) return true;

            return false;
        }

        // Check if user has opted-in to use managed memory
        static bool UseHmm()
        {
            if (getenv("RCCL_USE_HMM") == nullptr)
            {
                return false;
            }

            if (strcmp(getenv("RCCL_USE_HMM"), "1") == 0)
            {
                return true;
            }
            return false;
        }

        // Helper for HMM allocations: if device supports managedMemory, and HMM is requested through
        // RCCL_USE_HMM environment variable
        template <class T>
        hipError_t hipMallocHelper(T** devPtr, size_t size)
        {
            if (SupportsHmm() && UseHmm())
            {
                return hipMallocManaged((void**)devPtr, size);
            }
            else
            {
                return hipMalloc((void**)devPtr, size);
            }
            return hipSuccess;
        }

        // To be used in multi-process tests, in the parent process before forking children.
        void InitializeRootProcess(int            const numDevices_,
                                   size_t         const numElements_,
                                   ncclDataType_t const dataType_,
                                   bool           const inPlace_,
                                   ncclFunc_t     const func_ = ncclCollBroadcast)
        {
            numDevices  = numDevices_;
            numElements = numElements_;
            dataType    = dataType_;
            inPlace     = inPlace_;
            function    = func_;

            inputs.resize(numDevices);
            outputs.resize(numDevices);
            expected.resize(numDevices);

            for (int i = 0; i < numDevices_; i++)
            {
                inputs[i] = (void*)mmap(NULL, sizeof(void*), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
            }
            for (int i = 0; i < numDevices_; i++)
            {
                outputs[i] = (void*)mmap(NULL, sizeof(void*), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
            }
            for (int i = 0; i < numDevices_; i++)
            {
                expected[i] = (void*)mmap(NULL, NumBytes(ncclOutputBuffer), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
            }
        }

        void Initialize(int            const numDevices_,
                        size_t         const numElements_,
                        ncclDataType_t const dataType_,
                        bool           const inPlace_,
                        ncclFunc_t     const func_ = ncclCollBroadcast,
                        int            const multiProcessRank_ = -1)
        {
            numDevices  = numDevices_;
            numElements = numElements_;
            dataType    = dataType_;
            inPlace     = inPlace_;
            function    = func_;

            if (multiProcessRank_ == -1)
            {
                inputs.resize(numDevices);
                outputs.resize(numDevices);
                expected.resize(numDevices);
            }

            // Allocate per-device memory
            if (multiProcessRank_ > -1)
            {
                HIP_CALL(hipSetDevice(multiProcessRank_));
                HIP_CALL(hipMallocHelper((void **)&inputs[multiProcessRank_], NumBytes(ncclInputBuffer)));
                if (inPlace)
                    outputs[multiProcessRank_] = inputs[multiProcessRank_];
                else
                    HIP_CALL(hipMallocHelper((void **)&outputs[multiProcessRank_], NumBytes(ncclOutputBuffer)));
            }
            else
            {
              for (int i = 0; i < numDevices; i++)
              {
                  HIP_CALL(hipSetDevice(i));
                  HIP_CALL(hipMallocHelper((void **)&inputs[i], NumBytes(ncclInputBuffer)));
                  if (inPlace)
                      outputs[i] = inputs[i];
                  else
                      HIP_CALL(hipMallocHelper((void **)&outputs[i], NumBytes(ncclOutputBuffer)));

                  expected[i] = malloc(NumBytes(ncclOutputBuffer));
              }
            }

        }

        // Explicit memory release to avoid double-free from subDatasets
        void Release()
        {
            for (int i = 0; i < numDevices; i++)
            {
                if (!inPlace) hipFree(outputs[i]);
                hipFree(inputs[i]);
                free(expected[i]);
            }

            outputs.clear();
        }

        // Multi-process version of Release() where each process frees its own data
        void Release(int rank)
        {
            if (!inPlace) hipFree(outputs[rank]);
            hipFree(inputs[rank]);
        }

        void ReleaseRootProcess()
        {
            for (int i = 0; i < numDevices; i++)
            {
                munmap(inputs[i], sizeof(void*));
                munmap(outputs[i], sizeof(void*));
                munmap(expected[i], NumBytes(ncclOutputBuffer));
            }
            inputs.clear();
            outputs.clear();
            expected.clear();
        }

        // Creates a dataset by pointing to an existing dataset
        // Primarily to allow for testing with different starting byte-alignments
        void ExtractSubDataset(size_t const startElement,
                               size_t const lastElement,
                               Dataset& subDataset,
                               int const multiProcessRank = -1)
        {
            ASSERT_LE(startElement, lastElement);
            ASSERT_LT(lastElement, numElements);

            subDataset.numDevices  = numDevices;
            subDataset.numElements = lastElement - startElement + 1;
            subDataset.dataType    = dataType;
            subDataset.inPlace     = inPlace;
            subDataset.function    = function;

            subDataset.inputs.resize(numDevices);
            subDataset.outputs.resize(numDevices);
            subDataset.expected.resize(numDevices);

            size_t const byteOffset = (startElement * DataTypeToBytes(dataType));
            if (multiProcessRank != -1)
            {
                subDataset.inputs[multiProcessRank]   = (int8_t *)inputs[multiProcessRank] + byteOffset;
                subDataset.outputs[multiProcessRank]  = (int8_t *)outputs[multiProcessRank] + byteOffset;
                subDataset.expected[multiProcessRank] = (int8_t *)expected[multiProcessRank] + byteOffset;
            }
            else
            {
              for (int i = 0; i < numDevices; i++)
              {
                  subDataset.inputs[i]   = (int8_t *)inputs[i] + byteOffset;
                  subDataset.outputs[i]  = (int8_t *)outputs[i] + byteOffset;
                  subDataset.expected[i] = (int8_t *)expected[i] + byteOffset;
              }
            }
        }
    };

    class Barrier
    {
    public:
        Barrier(){};

        Barrier(int rank, int numRanks, int uniqueId)
        {
            this->numRanks = numRanks;
            std::string uniqueIdString = std::to_string(uniqueId);
            mutexName = std::string("mutex").append(uniqueIdString);
            turnstile1Name = std::string("turnstile1").append(uniqueIdString);
            turnstile2Name = std::string("turnstile2").append(uniqueIdString);
            counterName = std::string("counter").append(uniqueIdString);
            tinyBarrierName = std::string("tinyBarrier").append(uniqueIdString);

            size_t smSize = sizeof(sem_t);

            if (rank == 0)
            {
                NCCLCHECK_BARRIER_TEST(InitSemaphore(smSize, mutexName, 1, mutex), "InitSemaphore", rank);
                NCCLCHECK_BARRIER_TEST(InitSemaphore(smSize, turnstile1Name, 0, turnstile1), "InitSemaphore", rank);
                NCCLCHECK_BARRIER_TEST(InitSemaphore(smSize, turnstile2Name, 0, turnstile2), "InitSemaphore", rank);
                NCCLCHECK_BARRIER_TEST(OpenSharedMemoryVariable(sizeof(int), counterName, true, counter), "OpenSharedMemoryVariable", rank);
                NCCLCHECK_BARRIER_TEST(OpenSharedMemoryVariable(smSize, tinyBarrierName, true, tinyBarrier), "OpenSharedMemoryVariable", rank);
            }
            else
            {
                NCCLCHECK_BARRIER_TEST(OpenSharedMemoryVariable(smSize, tinyBarrierName, false, tinyBarrier), "OpenSharedMemoryVariable", rank);
                NCCLCHECK_BARRIER_TEST(OpenSemaphore(smSize, mutexName, mutex), "OpenSemaphore", rank);
                NCCLCHECK_BARRIER_TEST(OpenSemaphore(smSize, turnstile1Name, turnstile1), "OpenSemaphore", rank);
                NCCLCHECK_BARRIER_TEST(OpenSemaphore(smSize, turnstile2Name, turnstile2), "OpenSemaphore", rank);
                NCCLCHECK_BARRIER_TEST(OpenSharedMemoryVariable(sizeof(int), counterName, false, counter), "OpenSharedMemoryVariable", rank);
            }
            ncclResult_t res = Wait(20);
            if (res != ncclSuccess)
            {
                printf("Rank %d timed out during Barrier initialization.\n", rank);
            }
            ClearShmFiles(uniqueId);
        }

        // Wait with no timeout
        void Wait()
        {
            Part1();
            Part2();
        }

        // Wait with timeout option
        ncclResult_t Wait(int timeoutSecs)
        {
            NCCLCHECK_TEST(Part1(timeoutSecs), "Part 1 of Barrier Wait");
            NCCLCHECK_TEST(Part2(timeoutSecs), "Part 2 of Barrier Wait");

            return ncclSuccess;
        }

        ~Barrier()
        {
            size_t smSize = sizeof(sem_t);
            munmap(mutex, smSize);
            munmap(turnstile1, smSize);
            munmap(turnstile2, smSize);
            munmap(tinyBarrier, smSize);
            munmap(counter, sizeof(int));
        }

        static void ClearShmFiles(int uniqueId)
        {
            std::string uniqueIdString = std::to_string(uniqueId);
            std::vector<std::string> names;
            names.push_back(std::string("mutex").append(uniqueIdString));
            names.push_back(std::string("turnstile1").append(uniqueIdString));
            names.push_back(std::string("turnstile2").append(uniqueIdString));
            names.push_back(std::string("counter").append(uniqueIdString));
            names.push_back(std::string("tinyBarrier").append(uniqueIdString));

            std::string shmDir = "/dev/shm/";
            for (auto it = names.begin(); it != names.end(); it++)
            {
                struct stat fileStatus;
                std::string shmFullPath = shmDir + *it;

                // Check if shm file already exists; if so, unlink it
                if (stat(shmFullPath.c_str(), &fileStatus) == 0)
                {
                    shm_unlink(it->c_str());
                }
            }
        }
    private:
        template <typename T>
        ncclResult_t OpenSharedMemoryVariable(size_t size, std::string name, bool create, T& val)
        {
            int protection = PROT_READ | PROT_WRITE;
            int visibility = MAP_SHARED;
            int fd;

            std::string msg_open("shm_open ");
            msg_open.append(name);
            if (create)
            {
                SYSCHECKVAL_TEST(shm_open(name.c_str(), O_CREAT | O_RDWR, S_IRUSR | S_IWUSR), msg_open.c_str(), fd);
                SYSCHECK_GOTO_TEST(ftruncate(fd, size), "ftruncate", dropback);
            }
            else
            {
                do
                {
                    fd = shm_open(name.c_str(), O_RDWR, S_IRUSR | S_IWUSR);
                } while (fd == -1 && errno == ENOENT);
                if (fd == -1 && errno != ENOENT)
                {
                    printf("Call to %s failed: %s\n", msg_open.c_str(), strerror(errno));
                    return ncclSystemError;
                }
            }
            val = (T)mmap(NULL, size, protection, visibility, fd, 0);
            close(fd);
            if (val == MAP_FAILED)
            {
                goto dropback;
            }

            return ncclSuccess;
dropback:
            std::string msg_unlink("shm_unlink ");
            msg_unlink.append(name);
            SYSCHECK_TEST(shm_unlink(name.c_str()), "shm_unlink");
            return ncclSystemError;
        }

        ncclResult_t InitSemaphore(size_t size, std::string name, int semValue, sem_t*& semaphore)
        {
            ncclResult_t res = OpenSharedMemoryVariable<sem_t*>(size, name, true, semaphore);
            std::string msg_init("sem_init ");
            msg_init.append(name);
            SYSCHECK_TEST(sem_init(semaphore, 1, semValue), "sem_init");

            return res;
        }

        ncclResult_t OpenSemaphore(size_t size, std::string name, sem_t*& semaphore)
        {
            return OpenSharedMemoryVariable<sem_t*>(size, name, false, semaphore);
        }

        void Part1()
        {
            sem_wait(mutex);
            if (++(*counter) == numRanks)
            {
                sem_post_batch(turnstile1, numRanks);
            }
            sem_post(mutex);
            sem_wait(turnstile1);
        }

        void Part2()
        {
            sem_wait(mutex);
            if (--(*counter) == 0)
            {
                sem_post_batch(turnstile2, numRanks);
            }
            sem_post(mutex);
            sem_wait(turnstile2);
        }

        ncclResult_t Part1(int timeoutSecs)
        {
            struct timespec ts;
            SYSCHECK_TEST(clock_gettime(CLOCK_REALTIME, &ts), "clock_gettime 1");
            ts.tv_sec += timeoutSecs;

            SYSCHECK_TEST(sem_timedwait(mutex, &ts), "sem_timedwait 1-1");
            if (++(*counter) == numRanks)
            {
                SYSCHECK_TEST(sem_post_batch(turnstile1, numRanks), "sem_post_batch 1");
            }
            SYSCHECK_TEST(sem_post(mutex), "sem_post 1");
            SYSCHECK_TEST(sem_timedwait(turnstile1, &ts), "sem_timedwait 1-2");

            return ncclSuccess;
        }

        ncclResult_t Part2(int timeoutSecs)
        {
            struct timespec ts;
            SYSCHECK_TEST(clock_gettime(CLOCK_REALTIME, &ts), "clock_gettime 2");
            ts.tv_sec += timeoutSecs;

            SYSCHECK_TEST(sem_timedwait(mutex, &ts), "sem_timedwait 2");
            if (--(*counter) == 0)
            {
                SYSCHECK_TEST(sem_post_batch(turnstile2, numRanks), "sem_post_batch 2");
            }
            SYSCHECK_TEST(sem_post(mutex), "sem_post 2");
            SYSCHECK_TEST(sem_timedwait(turnstile2, &ts), "sem_timedwait 2-2");

            return ncclSuccess;
        }

        int sem_post_batch(sem_t*& sem, int n)
        {
            int ret = 0;
            for (int i = 0; i < n; i++)
            {
                ret = sem_post(sem);
                if (ret != 0) break;
            }

            return ret;
        }
        int numRanks;

        int* counter;

        sem_t* mutex;
        sem_t* turnstile1;
        sem_t* turnstile2;
        sem_t* tinyBarrier;

        std::string mutexName;
        std::string turnstile1Name;
        std::string turnstile2Name;
        std::string tinyBarrierName;
        std::string counterName;
    };

    typedef std::tuple<ncclRedOp_t    /* op          */,
                       ncclDataType_t /* dataType    */,
                       size_t         /* numElements */,
                       int            /* numDevices  */,
                       bool           /* inPlace     */,
                       const char*    /* envVals     */> TestTuple;

    // Base class for each collective test
    // - Each test is instantiated with a different TestTuple
    class CorrectnessTest : public testing::TestWithParam<TestTuple>
    {
    public:
        struct PrintToStringParamName
        {
            std::string operator()(const testing::TestParamInfo<CorrectnessTest::ParamType>& info)
            {
                std::string name;

                name += opStrings[std::get<0>(info.param)] + "_";
                name += dataTypeStrings[std::get<1>(info.param)] + "_";
                name += std::to_string(std::get<2>(info.param)) + "elements_";
                name += std::to_string(std::get<3>(info.param)) + "devices_";
                name += std::get<4>(info.param) == true ? "inplace_" : "outofplace_";
                std::string envVars = std::string(std::get<5>(info.param));
                std::replace(envVars.begin(), envVars.end(), '=', '_');
                name += envVars;

                return name;
            }

            std::map<ncclRedOp_t, std::string> opStrings
            {
                {ncclSum,  "sum"},
                {ncclProd, "prod"},
                {ncclMax,  "max"},
                {ncclMin,  "min"},
                {ncclAvg,  "avg"}
            };
            std::map<ncclDataType_t, std::string> dataTypeStrings
            {
                {ncclInt8,     "int8"},
                {ncclChar,     "char"},
                {ncclUint8,    "uint8"},
                {ncclInt32,    "int32"},
                {ncclInt,      "int"},
                {ncclUint32,   "uint32"},
                {ncclInt64,    "int64"},
                {ncclUint64,   "uint64"},
                {ncclFloat16,  "float16"},
                {ncclHalf,     "half"},
                {ncclFloat32,  "float32"},
                {ncclFloat64,  "float64"},
                {ncclDouble,   "double"},
                {ncclBfloat16, "bfloat16"}
            };
        };
    protected:
        // This code is called per test-tuple
        void SetUp() override
        {
            // Make the test tuple parameters accessible
            std::tie(op, dataType, numElements, numDevices, inPlace, envVals) = GetParam();

            // Collect the number of available GPUs
            HIP_CALL(hipGetDeviceCount(&numDevicesAvailable));

            // Only proceed with testing if there are enough GPUs
            if (numDevices > numDevicesAvailable)
            {
                fprintf(stdout, "[  SKIPPED ] Test requires %d devices (only %d available)\n",
                        numDevices, numDevicesAvailable);
                GTEST_SKIP();
            }

            bool enableClique = false;
            envString = 0;
            numTokens = 0;
            setenv("RCCL_TEST_ENV_VARS", "ENABLE", 1);
            if (strcmp(envVals, "")) {
                // enable RCCL env vars testing
                envString = strdup(envVals);
                tokens[numTokens] = strtok(envString, "=, ");
                numTokens++;
                while (tokens[numTokens-1] != NULL && numTokens < MAX_ENV_TOKENS)
                    tokens[numTokens++] = strtok(NULL, "=, ");
                for (int i = 0; i < numTokens/2; i++) {
                    char *val = getenv(tokens[i*2]);
                    if (val)
                        savedEnv[i] = strdup(val);
                    else
                        savedEnv[i] = 0;
                    setenv(tokens[i*2], tokens[i*2+1], 1);
                    fprintf(stdout, "[          ] setting environmental variable %s to %s\n", tokens[i*2], getenv(tokens[i*2]));
                    if (strcmp(tokens[i*2], "RCCL_ENABLE_CLIQUE") == 0)
                    {
                        if (strcmp(getenv(tokens[i*2]), "1") == 0)
                        {
                            enableClique = true;
                        }
                    }
                }
            }

            if (Dataset::UseHmm() && enableClique)
            {
                fprintf(stdout, "[  SKIPPED ] Clique mode and unified memory together not supported\n");
                GTEST_SKIP();
            }

            // Initialize communicators
            comms.resize(numDevices);
            NCCL_CALL(ncclCommInitAll(comms.data(), numDevices, NULL));

            // Create streams
            streams.resize(numDevices);
            for (int i = 0; i < numDevices; i++)
            {
                HIP_CALL(hipSetDevice(i));
                HIP_CALL(hipStreamCreate(&streams[i]));
            }
        }

        // Clean up per TestTuple
        void TearDown() override
        {
            if (IsSkipped()) return;

            // Release communicators and streams
            for (int i = 0; i < numDevices; i++)
            {
                NCCL_CALL(ncclCommDestroy(comms[i]));
                HIP_CALL(hipStreamDestroy(streams[i]));
            }
            // Restore env vars after tests
            for (int i = 0; i < numTokens/2; i++) {
                if (savedEnv[i]) {
                    setenv(tokens[i*2], savedEnv[i], 1);
                    fprintf(stdout, "[          ] restored environmental variable %s to %s\n", tokens[i*2], getenv(tokens[i*2]));
                    free(savedEnv[i]);
                }
                else {
                    unsetenv(tokens[i*2]);
                    fprintf(stdout, "[          ] removed environmental variable %s\n", tokens[i*2]);
                }
            }
            // Cleanup
            unsetenv("RCCL_TEST_ENV_VARS");
            free(envString);
        }

        void FillDatasetWithPattern(Dataset& dataset)
        {
            int8_t*   arrayI1 = (int8_t   *)malloc(dataset.NumBytes(ncclInputBuffer));
            uint8_t*  arrayU1 = (uint8_t  *)arrayI1;
            int32_t*  arrayI4 = (int32_t  *)arrayI1;
            uint32_t* arrayU4 = (uint32_t *)arrayI1;
            int64_t*  arrayI8 = (int64_t  *)arrayI1;
            uint64_t* arrayU8 = (uint64_t *)arrayI1;
            float*    arrayF4 = (float    *)arrayI1;
            double*   arrayF8 = (double   *)arrayI1;
            rccl_bfloat16* arrayB2 = (rccl_bfloat16 *)arrayI1;

            // NOTE: Currently half-precision float tests are unsupported due to half being supported
            //       on GPU only and not host

            // Fills input  data[i][j] with (i + j) % 256
            // - Keeping range small to reduce likelihood of overflow
            // - Sticking with floating points values that are perfectly representable
            for (int i = 0; i < dataset.numDevices; i++)
            {
                for (int j = 0; j < dataset.NumBytes(ncclInputBuffer)/DataTypeToBytes(dataset.dataType); j++)
                {
                    int    valueI = (i + j) % 256;
                    double  valueF = 1.0L/((double)valueI+1.0L);

                    switch (dataset.dataType)
                    {
                    case ncclInt8:    arrayI1[j] = valueI; break;
                    case ncclUint8:   arrayU1[j] = valueI; break;
                    case ncclInt32:   arrayI4[j] = valueI; break;
                    case ncclUint32:  arrayU4[j] = valueI; break;
                    case ncclInt64:   arrayI8[j] = valueI; break;
                    case ncclUint64:  arrayU8[j] = valueI; break;
                    case ncclFloat32: arrayF4[j] = valueF; break;
                    case ncclFloat64: arrayF8[j] = valueF; break;
                    case ncclBfloat16: arrayB2[j] = rccl_bfloat16(valueF); break;
                    default:
                        fprintf(stderr, "[ERROR] Unsupported datatype\n");
                        exit(0);
                    }
                }

                HIP_CALL(hipSetDevice(i));
                HIP_CALL(hipMemcpy(dataset.inputs[i], arrayI1, dataset.NumBytes(ncclInputBuffer), hipMemcpyHostToDevice));

                // Fills output data[i][j] with 0 (if not inplace)
                if (!dataset.inPlace)
                    HIP_CALL(hipMemset(dataset.outputs[i], 0, dataset.NumBytes(ncclOutputBuffer)));
            }

            free(arrayI1);
        }

        void Synchronize() const
        {
            // Wait for reduction to complete
            for (int i = 0; i < numDevices; i++)
            {
                HIP_CALL(hipSetDevice(i));
                HIP_CALL(hipStreamSynchronize(streams[i]));
            }
        }

        static void Average(Dataset const& dataset, int8_t* resultI1)
        {
            uint8_t*  resultU1 = (uint8_t  *)resultI1;
            int32_t*  resultI4 = (int32_t  *)resultI1;
            uint32_t* resultU4 = (uint32_t *)resultI1;
            int64_t*  resultI8 = (int64_t  *)resultI1;
            uint64_t* resultU8 = (uint64_t *)resultI1;
            float*    resultF4 = (float    *)resultI1;
            double*   resultF8 = (double   *)resultI1;
            rccl_bfloat16* resultB2 = (rccl_bfloat16 *)resultI1;
            for (int j = 0; j < dataset.numElements; j++)
            {
                switch (dataset.dataType)
                {
                case ncclInt8:     resultI1[j] = resultI1[j]/dataset.numDevices; break;
                case ncclUint8:    resultU1[j] = resultU1[j]/dataset.numDevices; break;
                case ncclInt32:    resultI4[j] = resultI4[j]/dataset.numDevices; break;
                case ncclUint32:   resultU4[j] = resultU4[j]/dataset.numDevices; break;
                case ncclInt64:    resultI8[j] = resultI8[j]/dataset.numDevices; break;
                case ncclUint64:   resultU8[j] = resultU8[j]/dataset.numDevices; break;
                case ncclFloat32:  resultF4[j] = resultF4[j]/dataset.numDevices; break;
                case ncclFloat64:  resultF8[j] = resultF8[j]/dataset.numDevices; break;
                case ncclBfloat16: resultB2[j] = rccl_bfloat16((float)(resultB2[j])/dataset.numDevices); break;
                default:
                    fprintf(stderr, "[ERROR] Unsupported datatype\n");
                    exit(0);
                }
            }
        }

        void ValidateResults(Dataset const& dataset, int root = 0) const
        {
            int8_t*   outputI1 = (int8_t   *)malloc(dataset.NumBytes(ncclOutputBuffer));
            uint8_t*  outputU1 = (uint8_t  *)outputI1;
            int32_t*  outputI4 = (int32_t  *)outputI1;
            uint32_t* outputU4 = (uint32_t *)outputI1;
            int64_t*  outputI8 = (int64_t  *)outputI1;
            uint64_t* outputU8 = (uint64_t *)outputI1;
            float*    outputF4 = (float    *)outputI1;
            double*   outputF8 = (double   *)outputI1;
            rccl_bfloat16* outputB2 = (rccl_bfloat16 *)outputI1;

            bool isMatch = true;

            // Loop over each device's output and compare it to the expected output
            // (Each collective operation computes its own expected results)
            for (int i = 0; i < dataset.numDevices && isMatch; i++)
            {
                // only output on root rank is valid for gather collective
                if (dataset.function == ncclCollGather && i != root)
                    continue;
                HIP_CALL(hipMemcpy(outputI1, dataset.outputs[i], dataset.NumBytes(ncclOutputBuffer), hipMemcpyDeviceToHost));

                int8_t*   expectedI1 = (int8_t   *)dataset.expected[i];
                uint8_t*  expectedU1 = (uint8_t  *)expectedI1;
                int32_t*  expectedI4 = (int32_t  *)expectedI1;
                uint32_t* expectedU4 = (uint32_t *)expectedI1;
                int64_t*  expectedI8 = (int64_t  *)expectedI1;
                uint64_t* expectedU8 = (uint64_t *)expectedI1;
                float*    expectedF4 = (float    *)expectedI1;
                double*   expectedF8 = (double   *)expectedI1;
                rccl_bfloat16* expectedB2 = (rccl_bfloat16 *)expectedI1;

                for (int j = 0; j < dataset.numElements && isMatch; j++)
                {
                    switch (dataset.dataType)
                    {
                    case ncclInt8:    isMatch &= (outputI1[j] == expectedI1[j]); break;
                    case ncclUint8:   isMatch &= (outputU1[j] == expectedU1[j]); break;
                    case ncclInt32:   isMatch &= (outputI4[j] == expectedI4[j]); break;
                    case ncclUint32:  isMatch &= (outputU4[j] == expectedU4[j]); break;
                    case ncclInt64:   isMatch &= (outputI8[j] == expectedI8[j]); break;
                    case ncclUint64:  isMatch &= (outputU8[j] == expectedU8[j]); break;
                    case ncclFloat32: isMatch &= (fabs(outputF4[j] - expectedF4[j]) < 1e-5); break;
                    case ncclFloat64: isMatch &= (fabs(outputF8[j] - expectedF8[j]) < 1e-12); break;
                    case ncclBfloat16: isMatch &= (fabs((float)outputB2[j] - (float)expectedB2[j]) < 5e-2); break;
                    default:
                        fprintf(stderr, "[ERROR] Unsupported datatype\n");
                        exit(0);
                    }

                    if (!isMatch)
                    {
                        switch (dataset.dataType)
                        {
                        case ncclInt8:
                            printf("Expected %d.  Output %d on device %d[%d]\n", expectedI1[j], outputI1[j], i, j);
                            break;
                        case ncclUint8:
                            printf("Expected %u.  Output %u on device %d[%d]\n", expectedU1[j], outputU1[j], i, j); break;
                        case ncclInt32:
                            printf("Expected %d.  Output %d on device %d[%d]\n", expectedI4[j], outputI4[j], i, j); break;
                        case ncclUint32:
                            printf("Expected %u.  Output %u on device %d[%d]\n", expectedU4[j], outputU4[j], i, j); break;
                        case ncclInt64:
                            printf("Expected %ld.  Output %ld on device %d[%d]\n", expectedI8[j], outputI8[j], i, j); break;
                        case ncclUint64:
                            printf("Expected %lu.  Output %lu on device %d[%d]\n", expectedU8[j], outputU8[j], i, j); break;
                        case ncclFloat32:
                            printf("Expected %f.  Output %f on device %d[%d]\n", expectedF4[j], outputF4[j], i, j); break;
                        case ncclFloat64:
                            printf("Expected %lf.  Output %lf on device %d[%d]\n", expectedF8[j], outputF8[j], i, j); break;
                        case ncclBfloat16:
                            printf("Expected %f.  Output %f on device %d[%d]\n", (float)expectedB2[j], (float)outputB2[j], i, j); break;
                        default:
                            fprintf(stderr, "[ERROR] Unsupported datatype\n");
                            exit(0);
                        }
                    }
                }
                ASSERT_EQ(isMatch, true);
            }
            free(outputI1);
        }

        // Passed in parameters from TestTuple
        ncclRedOp_t              op;
        ncclDataType_t           dataType;
        size_t                   numElements;
        int                      numDevices;
        bool                     inPlace;
        const char*              envVals;

        int                      numDevicesAvailable;
        std::vector<ncclComm_t>  comms;
        std::vector<hipStream_t> streams;

        // internal only
        char*                    envString;
        char*                    tokens[MAX_ENV_TOKENS];
        int                      numTokens;
        char*                    savedEnv[MAX_ENV_TOKENS/2];
    };

    class MultiProcessCorrectnessTest : public CorrectnessTest
    {
    protected:
        // IMPORTANT: We cannot have any HIP API calls in the parent process.
        // Do any HIP setup in SetupPerProcess().
        void SetUp() override
        {
            // Check if NCCL_COMM_ID is already set; if not, set it now
            if (!getenv("NCCL_COMM_ID"))
            {
                char hostname[HOST_NAME_MAX+1];
                gethostname(hostname, HOST_NAME_MAX+1);
                std::string hostnameString(hostname);
                hostnameString.append(":55513");
                setenv("NCCL_COMM_ID", hostnameString.c_str(), 0);
            }

            // Make the test tuple parameters accessible
            std::tie(op, dataType, numElements, numDevices, inPlace, envVals) = GetParam();

            envString = 0;
            numTokens = 0;
            bool enableClique = false;

            setenv("RCCL_TEST_ENV_VARS", "ENABLE", 1);
            if (strcmp(envVals, "")) {
                // enable RCCL env vars testing
                envString = strdup(envVals);
                tokens[numTokens] = strtok(envString, "=, ");
                numTokens++;
                while (tokens[numTokens-1] != NULL && numTokens < MAX_ENV_TOKENS)
                    tokens[numTokens++] = strtok(NULL, "=, ");
                for (int i = 0; i < numTokens/2; i++) {
                    char *val = getenv(tokens[i*2]);
                    if (val)
                        savedEnv[i] = strdup(val);
                    else
                        savedEnv[i] = 0;
                    setenv(tokens[i*2], tokens[i*2+1], 1);
                    fprintf(stdout, "[          ] setting environmental variable %s to %s\n", tokens[i*2], getenv(tokens[i*2]));
                    if (strcmp(tokens[i*2], "RCCL_ENABLE_CLIQUE") == 0)
                    {
                        if (strcmp(getenv(tokens[i*2]), "1") == 0)
                        {
                            enableClique = true;
                        }
                    }
                }
            }

            if (Dataset::UseHmm() && enableClique)
            {
                fprintf(stdout, "[  SKIPPED ] Clique mode and unified memory together not supported\n");
                GTEST_SKIP();
            }

            comms.resize(numDevices);
            streams.resize(numDevices);
            dataset = (Dataset*)mmap(NULL, sizeof(Dataset), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
            Barrier::ClearShmFiles(StripPortNumberFromCommId(std::string(getenv("NCCL_COMM_ID"))));
        }

        void TearDown() override
        {
            munmap(dataset, sizeof(Dataset));

            // Restore env vars after tests
            for (int i = 0; i < numTokens/2; i++) {
                if (savedEnv[i]) {
                    setenv(tokens[i*2], savedEnv[i], 1);
                    fprintf(stdout, "[          ] restored environmental variable %s to %s\n", tokens[i*2], getenv(tokens[i*2]));
                    free(savedEnv[i]);
                }
                else {
                    unsetenv(tokens[i*2]);
                    fprintf(stdout, "[          ] removed environmental variable %s\n", tokens[i*2]);
                }
            }
            // Cleanup
            unsetenv("RCCL_TEST_ENV_VARS");
            free(envString);
        }

        void SetUpPerProcessHelper(int rank, ncclComm_t& comm, hipStream_t& stream)
        {
            // Check for NCCL_COMM_ID env variable (otherwise will not init)
            if (!getenv("NCCL_COMM_ID"))
            {
                printf("Must set NCCL_COMM_ID prior to execution\n");
                exit(0);
            }

            // Collect the number of available GPUs
            HIP_CALL(hipGetDeviceCount(&numDevicesAvailable));

            // Only proceed with testing if there are enough GPUs
            if (numDevices > numDevicesAvailable)
            {
                if (rank == 0)
                {
                    fprintf(stdout, "[  SKIPPED ] Test requires %d devices (only %d available)\n",
                            numDevices, numDevicesAvailable);
                }
                GTEST_SKIP();
            }

            HIP_CALL(hipSetDevice(rank));
            HIP_CALL(hipStreamCreate(&stream));

            ncclUniqueId id;
            NCCL_CALL(ncclGetUniqueId(&id));

            ncclResult_t res;
            res = ncclCommInitRank(&comm, numDevices, id, rank); // change to local comm and stream per process

            if (res != ncclSuccess)
            {
                printf("Test failure:%s %d '%s' numRanks:%d\n", __FILE__,__LINE__,ncclGetErrorString(res), numDevices);
                ASSERT_EQ(res, ncclSuccess);
            }
        }

        // To be called by each process individually
        void SetUpPerProcess(int rank, ncclFunc_t const func, ncclComm_t& comm, hipStream_t& stream, Dataset& dataset)
        {
            SetUpPerProcessHelper(rank, comm, stream);
            if (numDevices <= numDevicesAvailable)
            {
                dataset.Initialize(numDevices, numElements, dataType, inPlace, func, rank);
            }
        }

        // To be called by each process/rank individually (see GroupCallsMultiProcess)
        void SetUpPerProcess(int rank, std::vector<ncclFunc_t> const& func, ncclComm_t& comm, hipStream_t& stream, std::vector<Dataset*>& datasets)
        {
            SetUpPerProcessHelper(rank, comm, stream);
            if (numDevices <= numDevicesAvailable)
            {
                for (int i = 0; i < datasets.size(); i++)
                {
                    datasets[i]->Initialize(numDevices, numElements, dataType, inPlace, func[i], rank);
                }
            }
        }

        // Clean up per process
        void TearDownPerProcess(ncclComm_t& comm, hipStream_t& stream)
        {
            NCCL_CALL(ncclCommDestroy(comm));
            HIP_CALL(hipStreamDestroy(stream));
        }

        void FillDatasetWithPattern(Dataset& dataset, int rank)
        {
            int8_t*   arrayI1 = (int8_t   *)malloc(dataset.NumBytes(ncclInputBuffer));
            uint8_t*  arrayU1 = (uint8_t  *)arrayI1;
            int32_t*  arrayI4 = (int32_t  *)arrayI1;
            uint32_t* arrayU4 = (uint32_t *)arrayI1;
            int64_t*  arrayI8 = (int64_t  *)arrayI1;
            uint64_t* arrayU8 = (uint64_t *)arrayI1;
            float*    arrayF4 = (float    *)arrayI1;
            double*   arrayF8 = (double   *)arrayI1;
            rccl_bfloat16* arrayB2 = (rccl_bfloat16 *)arrayI1;

            // NOTE: Currently half-precision float tests are unsupported due to half being supported
            //       on GPU only and not host

            // Fills input  data[i][j] with (i + j) % 6
            // - Keeping range small to reduce likelihood of overflow
            // - Sticking with floating points values that are perfectly representable

            for (int j = 0; j < dataset.NumBytes(ncclInputBuffer)/DataTypeToBytes(dataset.dataType); j++)
            {
                int    valueI = (rank + j) % 6;
                float  valueF = (float)valueI;

                switch (dataset.dataType)
                {
                case ncclInt8:    arrayI1[j] = valueI; break;
                case ncclUint8:   arrayU1[j] = valueI; break;
                case ncclInt32:   arrayI4[j] = valueI; break;
                case ncclUint32:  arrayU4[j] = valueI; break;
                case ncclInt64:   arrayI8[j] = valueI; break;
                case ncclUint64:  arrayU8[j] = valueI; break;
                case ncclFloat32: arrayF4[j] = valueF; break;
                case ncclFloat64: arrayF8[j] = valueF; break;
                case ncclBfloat16: arrayB2[j] = rccl_bfloat16(valueF); break;
                default:
                    fprintf(stderr, "[ERROR] Unsupported datatype\n");
                    exit(0);
                }
            }

            HIP_CALL(hipSetDevice(rank));
            HIP_CALL(hipMemcpy(dataset.inputs[rank], arrayI1, dataset.NumBytes(ncclInputBuffer), hipMemcpyHostToDevice));

            // Fills output data[i][j] with 0 (if not inplace)
            if (!dataset.inPlace)
                HIP_CALL(hipMemset(dataset.outputs[rank], 0, dataset.NumBytes(ncclOutputBuffer)));

            free(arrayI1);
        }

        bool ValidateResults(Dataset const& dataset, int rank, int root = 0) const
        {
            int8_t*   outputI1 = (int8_t   *)malloc(dataset.NumBytes(ncclOutputBuffer));
            uint8_t*  outputU1 = (uint8_t  *)outputI1;
            int32_t*  outputI4 = (int32_t  *)outputI1;
            uint32_t* outputU4 = (uint32_t *)outputI1;
            int64_t*  outputI8 = (int64_t  *)outputI1;
            uint64_t* outputU8 = (uint64_t *)outputI1;
            float*    outputF4 = (float    *)outputI1;
            double*   outputF8 = (double   *)outputI1;
            rccl_bfloat16* outputB2 = (rccl_bfloat16 *)outputI1;

            bool isMatch = true;

            // Loop over each device's output and compare it to the expected output
            // (Each collective operation computes its own expected results)

            // only output on root rank is valid for gather collective
            if (dataset.function == ncclCollGather && rank != root)
                return true;

            hipError_t err = hipMemcpy(outputI1, dataset.outputs[rank], dataset.NumBytes(ncclOutputBuffer), hipMemcpyDeviceToHost);
            if (err != hipSuccess)
                return false;

            int8_t*   expectedI1 = (int8_t   *)dataset.expected[rank];
            uint8_t*  expectedU1 = (uint8_t  *)expectedI1;
            int32_t*  expectedI4 = (int32_t  *)expectedI1;
            uint32_t* expectedU4 = (uint32_t *)expectedI1;
            int64_t*  expectedI8 = (int64_t  *)expectedI1;
            uint64_t* expectedU8 = (uint64_t *)expectedI1;
            float*    expectedF4 = (float    *)expectedI1;
            double*   expectedF8 = (double   *)expectedI1;
            rccl_bfloat16* expectedB2 = (rccl_bfloat16 *)expectedI1;

            for (int j = 0; j < dataset.numElements && isMatch; j++)
            {
                switch (dataset.dataType)
                {
                case ncclInt8:    isMatch &= (outputI1[j] == expectedI1[j]); break;
                case ncclUint8:   isMatch &= (outputU1[j] == expectedU1[j]); break;
                case ncclInt32:   isMatch &= (outputI4[j] == expectedI4[j]); break;
                case ncclUint32:  isMatch &= (outputU4[j] == expectedU4[j]); break;
                case ncclInt64:   isMatch &= (outputI8[j] == expectedI8[j]); break;
                case ncclUint64:  isMatch &= (outputU8[j] == expectedU8[j]); break;
                case ncclFloat32: isMatch &= (outputF4[j] == expectedF4[j]); break;
                case ncclFloat64: isMatch &= (outputF8[j] == expectedF8[j]); break;
                case ncclBfloat16: isMatch &= (outputB2[j] == expectedB2[j]); break;
                default:
                    fprintf(stderr, "[ERROR] Unsupported datatype\n");
                    exit(0);
                }

                if (!isMatch)
                {
                    switch (dataset.dataType)
                    {
                    case ncclInt8:
                        printf("Output %d.  Expected %d on device %d[%d]\n", outputI1[j], expectedI1[j], rank, j); break;
                    case ncclUint8:
                        printf("Output %u.  Expected %u on device %d[%d]\n", outputU1[j], expectedU1[j], rank, j); break;
                    case ncclInt32:
                        printf("Output %d.  Expected %d on device %d[%d]\n", outputI4[j], expectedI4[j], rank, j); break;
                    case ncclUint32:
                        printf("Output %u.  Expected %u on device %d[%d]\n", outputU4[j], expectedU4[j], rank, j); break;
                    case ncclInt64:
                        printf("Output %ld.  Expected %ld on device %d[%d]\n", outputI8[j], expectedI8[j], rank, j); break;
                    case ncclUint64:
                        printf("Output %lu.  Expected %lu on device %d[%d]\n", outputU8[j], expectedU8[j], rank, j); break;
                    case ncclFloat32:
                        printf("Output %f.  Expected %f on device %d[%d]\n", outputF4[j], expectedF4[j], rank, j); break;
                    case ncclFloat64:
                        printf("Output %lf.  Expected %lf on device %d[%d]\n", outputF8[j], expectedF8[j], rank, j); break;
                    case ncclBfloat16:
                        printf("Output %f.  Expected %f on device %d[%d]\n", (float)outputB2[j], (float)expectedB2[j], rank, j); break;
                    default:
                        fprintf(stderr, "[ERROR] Unsupported datatype\n");
                        exit(0);
                    }
                }
            }
            return isMatch;
        }

        void ValidateProcesses(std::vector<int> const& pids)
        {
            int numProcesses = pids.size();
            int status[numProcesses];

            for (int i = 0; i < numProcesses; i++)
            {
                waitpid(pids[i], &status[i], 0);

                EXPECT_NE(WIFEXITED(status[i]), 0) << "[ERROR] Child process " << i << " did not exit cleanly.";
                EXPECT_EQ(WEXITSTATUS(status[i]), EXIT_SUCCESS) << "[ERROR] Child process " << i << " had a test failure.";
            }
        }

        void TerminateChildProcess(bool const pass)
        {
            if (pass)
            {
                exit(EXIT_SUCCESS);
            }
            else
            {
                exit(EXIT_FAILURE);
            }
        }

        int StripPortNumberFromCommId(std::string commId)
        {
            size_t pos = commId.find(":");
            std::string portNumString = commId.substr(pos + 1);
            return std::atoi(portNumString.c_str());
        }

        Dataset* dataset;
    };

    std::string GenerateTestNameString(testing::TestParamInfo<MultiProcessCorrectnessTest::ParamType>& info);
}

#endif
