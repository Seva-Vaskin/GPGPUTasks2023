#include <libutils/misc.h>
#include <libutils/timer.h>
#include <libutils/fast_random.h>
#include "libgpu/context.h"
#include "libgpu/shared_device_buffer.h"

#include "cl/sum_cl.h"


template<typename T>
void raiseFail(const T &a, const T &b, std::string message, std::string filename, int line) {
    if (a != b) {
        std::cerr << message << " But " << a << " != " << b << ", " << filename << ":" << line << std::endl;
        throw std::runtime_error(message);
    }
}

#define EXPECT_THE_SAME(a, b, message) raiseFail(a, b, message, __FILE__, __LINE__)


int main(int argc, char **argv) {
    int benchmarkingIters = 10;

    unsigned int reference_sum = 0;
    unsigned int n = 100 * 1000 * 1000;
    std::vector<unsigned int> as(n, 0);
    FastRandom r(42);
    for (int i = 0; i < n; ++i) {
        as[i] = (unsigned int) r.next(0, std::numeric_limits<unsigned int>::max() / n);
        reference_sum += as[i];
    }

    {
        timer t;
        for (int iter = 0; iter < benchmarkingIters; ++iter) {
            unsigned int sum = 0;
            for (int i = 0; i < n; ++i) {
                sum += as[i];
            }
            EXPECT_THE_SAME(reference_sum, sum, "CPU result should be consistent!");
            t.nextLap();
        }
        std::cout << "CPU:     " << t.lapAvg() << "+-" << t.lapStd() << " s" << std::endl;
        std::cout << "CPU:     " << (n / 1000.0 / 1000.0) / t.lapAvg() << " millions/s" << std::endl;
    }

    {
        timer t;
        for (int iter = 0; iter < benchmarkingIters; ++iter) {
            unsigned int sum = 0;
#pragma omp parallel for reduction(+:sum)
            for (int i = 0; i < n; ++i) {
                sum += as[i];
            }
            EXPECT_THE_SAME(reference_sum, sum, "CPU OpenMP result should be consistent!");
            t.nextLap();
        }
        std::cout << "CPU OMP: " << t.lapAvg() << "+-" << t.lapStd() << " s" << std::endl;
        std::cout << "CPU OMP: " << (n / 1000.0 / 1000.0) / t.lapAvg() << " millions/s" << std::endl;
    }

    {
        gpu::Device device = gpu::chooseGPUDevice(argc, argv);

        gpu::Context context;
        context.init(device.device_id_opencl);
        context.activate();


        auto benchmarkKernel = [&](const std::string &kernelName, unsigned int workGroupSize,
                                   unsigned int valuesPerWorkItem, unsigned int workgroupSize) {
            gpu::gpu_mem_32u as_gpu;
            as_gpu.resizeN(n);
            as_gpu.writeN(as.data(), n);

            ocl::Kernel kernel(sum_kernel, sum_kernel_length, kernelName,
                               "-D VALUES_PER_WORKITEM=" + to_string(valuesPerWorkItem) +
                               " -D WORKGROUP_SIZE=" + to_string(workGroupSize));
            kernel.compile();

            unsigned int globalWorkSize;
            if (valuesPerWorkItem != 0)
                globalWorkSize = gpu::divup(n, valuesPerWorkItem);
            else
                globalWorkSize = n;

            timer t;
            for (int iter = 0; iter < benchmarkingIters; ++iter) {
                gpu::gpu_mem_32u res_gpu = gpu::gpu_mem_32u::createN(1);
                unsigned int zero = 0;
                res_gpu.writeN(&zero, 1);

                kernel.exec(gpu::WorkSize(workGroupSize, globalWorkSize), as_gpu, n, res_gpu);

                unsigned int sum;
                res_gpu.readN(&sum, 1);

                EXPECT_THE_SAME(reference_sum, sum, std::string(kernelName) +" result should be consistent!");
                t.nextLap();
            }
            std::cout << "GPU " << kernelName << ": " << t.lapAvg() << "+-" << t.lapStd() << " s" << std::endl;
            std::cout << "GPU " << kernelName << ": " << (n / 1000.0 / 1000.0) / t.lapAvg() << " millions/s"
                      << std::endl;
        };

        benchmarkKernel("sum_global_atomic", 128, 0, 0);
        benchmarkKernel("sum_loop", 128, 128, 0);
        benchmarkKernel("sum_loop_coalesced", 128, 128, 0);
        benchmarkKernel("sum_local_mem", 128, 0, 128);
        benchmarkKernel("sum_local_mem_and_tree", 128, 0, 128);
    }
}