#include "CpuUsage.h"
#include "Rct.h"
#include <thread>
#include <mutex>
#include <unistd.h>
#include <assert.h>
#ifdef OS_Darwin
#include <sys/sysctl.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <mach/mach_host.h>
#endif

#define SLEEP_TIME 1000000 // one second

struct CpuData
{
    std::mutex mutex;
    std::thread thread;

    uint32_t lastUsage;
    uint64_t lastTime;

    float usage;

#if defined(OS_Linux) || defined (OS_Darwin)
    float hz;
    uint32_t cores;
#endif
};

static CpuData sData;
static std::once_flag sFlag;

static int64_t currentUsage()
{
#if defined(OS_Linux)
    FILE* f = fopen("/proc/stat", "r");
    if (!f)
        return -1;
    char cpu[20];
    uint32_t user, nice, system, idle;
    if (fscanf(f, "%s\t%u\t%u\t%u\t%u\t", cpu, &user, &nice, &system, &idle) != 5) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return idle;
#elif defined(OS_Darwin)
    processor_info_array_t cpuInfo;
    mach_msg_type_number_t numCpuInfo;
    natural_t numCPUs = 0;
    kern_return_t err = host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &numCPUs, &cpuInfo, &numCpuInfo);
    if (err == KERN_SUCCESS) {
        int64_t usage = 0;
        for (unsigned int i = 0; i < numCPUs; ++i) {
            usage += cpuInfo[(CPU_STATE_MAX * i) + CPU_STATE_USER] + cpuInfo[(CPU_STATE_MAX * i) + CPU_STATE_SYSTEM] + cpuInfo[(CPU_STATE_MAX * i) + CPU_STATE_NICE];
        }
        const size_t cpuInfoSize = sizeof(integer_t) * numCpuInfo;
        vm_deallocate(mach_task_self(), (vm_address_t)cpuInfo, cpuInfoSize);
        return usage;
    }
    return -1;
#else
#warning "CpuUsage not implemented for this platform"
    return -1;
#endif
}

static void collectData()
{
    for (;;) {
        const int64_t usage = currentUsage();
        if (usage == -1)
            break;
        const uint64_t time = Rct::monoMs();

        {
            std::lock_guard<std::mutex> locker(sData.mutex);
            assert(sData.lastTime < time);
            if (sData.lastTime > 0) {
                // did we wrap? if so, make load be 1 for now
                if (sData.lastUsage > usage) {
                    sData.usage = 0;
                } else {
#if defined(OS_Linux) || defined(OS_Darwin)
                    const uint32_t deltaUsage = usage - sData.lastUsage;
                    const uint64_t deltaTime = time - sData.lastTime;
                    const float timeRatio = deltaTime / (SLEEP_TIME / 1000);
                    sData.usage = (deltaUsage / sData.hz / sData.cores) / timeRatio;
#endif
                }
            }
            sData.lastUsage = usage;
            sData.lastTime = time;
        }

        usleep(SLEEP_TIME);
    }
}

float CpuUsage::usage()
{
    std::call_once(sFlag, []() {
            std::lock_guard<std::mutex> locker(sData.mutex);
            sData.usage = 0;
            sData.lastUsage = 0;
            sData.lastTime = 0;
#if defined(OS_Linux) || defined(OS_Darwin)
            sData.hz = sysconf(_SC_CLK_TCK);
            sData.cores = sysconf(_SC_NPROCESSORS_ONLN);
#endif
            sData.thread = std::thread(collectData);
        });

    std::lock_guard<std::mutex> locker(sData.mutex);
    return 1. - sData.usage;
}
