// Copyright 2018 Chia Network Inc

// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//    http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef SRC_CPP_PLOTTER_DISK_HPP_
#define SRC_CPP_PLOTTER_DISK_HPP_

#ifndef _WIN32
#include <semaphore.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <math.h>
#include <stdio.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <memory>

#include "chia_filesystem.hpp"

#include "calculate_bucket.hpp"
#include "encoding.hpp"
#include "exceptions.hpp"
#include "phase1.hpp"
#include "phase2.hpp"
#include "b17phase2.hpp"
#include "phase3.hpp"
#include "b17phase3.hpp"
#include "phase4.hpp"
#include "b17phase4.hpp"
#include "pos_constants.hpp"
#include "sort_manager.hpp"
#include "util.hpp"

#define B17PHASE23

class DiskPlotter {
public:
    // This method creates a plot on disk with the filename. Many temporary files
    // (filename + ".table1.tmp", filename + ".p2.t3.sort_bucket_4.tmp", etc.) are created
    // and their total size will be larger than the final plot file. Temp files are deleted at the
    // end of the process.

    void CreatePlotDisk(
        std::string tmp_dirname,            // 临时文件存放路径
        std::string tmp2_dirname,           // 备用临时文件存放路径
        std::string final_dirname,          // 最终文件存放路径
        std::string filename,               // Plot文件名，示例(标识-k大小-日期-PlotId)：plot-k32-2021-05-02-10-56-072e5997602e5796b8966ac1e75044d2e81b2897ebab455de18211db12d79a23.plot
        uint8_t k,                          // K的大小
        const uint8_t* memo,                // memo字段，Plot的备忘录，示例(pool_public_key + farmer_public_key + sk，直接进行字节拼接)：b523cd9d58972af56ba6d5d61ccdf77e76894bafa5df3785055334e98e9b7dcacf21d41c491d2d876767df304e2742ae939df12309be853da848961b2089f9c3620622a1f2e49fd0fa74f228a006367000e58d3ded9df8004de5c54acada438032fb11d5e27393ddd4c66ec8ea5452b9dd20d91ecefc9a2d0c5dcccb0adec9c9
        uint32_t memo_len,                  // memeo字段的长度，48 + 48 + 32(详见plot_tools.py的stream_plot_info_pk方法，这个设计的为可变，具体意图不明)
        const uint8_t* id,                  // Plot文件的唯一Id(Plot Id)
        uint32_t id_len,                    // Id的长度，32(详见WriteHeader的id，这个在Chia设计的时候已经固定死了，不回发生变更)
        uint32_t buf_megabytes_input = 0,   // 创建Plot文件设置的缓冲区(内存)大小
        uint32_t num_buckets_input = 0,     // 创建Plot文件设置的桶的数量
        uint64_t stripe_size_input = 0,     // 创建Plot文件设置的条带深度
        uint8_t num_threads_input = 0,      // 创建Plots文件设置的现场数量
        bool nobitfield = false,            // 设置nobitfield
        bool show_progress = false)         // 显示进度
    {
        //增加打开文件的限制，我们会打开很多文件.
        // Increases the open file limit, we will open a lot of files.
#ifndef _WIN32
        struct rlimit the_limit = {600, 600};
        if (-1 == setrlimit(RLIMIT_NOFILE, &the_limit)) {
            std::cout << "setrlimit failed" << std::endl;
        }
#endif

        // 检查k的取值范围
        if (k < kMinPlotSize || k > kMaxPlotSize) {
            throw InvalidValueException("Plot size k= " + std::to_string(k) + " is invalid");
        }

        uint32_t stripe_size, buf_megabytes, num_buckets;
        uint8_t num_threads;

        // 如果输入条带深度设置为非0，则使用输入值作为条带深度。如果为0，则设置为65535(64K)，StripeSzie的解释可以查阅：https://blog.51cto.com/u_11511126/1974585
        if (stripe_size_input != 0) {
            stripe_size = stripe_size_input;
        } else {
            stripe_size = 65536;
        }

        // 如果输入线程数量为非0，则使用输入线程数作为线程数。如果为0，则设置为2。
        if (num_threads_input != 0) {
            num_threads = num_threads_input;
        } else {
            num_threads = 2;
        }

        // 如果输入缓冲区大小设置为非0，则使用输入缓冲区大小作为缓冲区大小。如果为0，则设置为4609，在1.1.x版本中，已经调整为3390MB了。
        if (buf_megabytes_input != 0) {
            buf_megabytes = buf_megabytes_input;
        } else {
            buf_megabytes = 4608;
        }

        // 缓冲区大小取值判断，小于10抛出异常
        if (buf_megabytes < 10) {
            throw InsufficientMemoryException("Please provide at least 10MiB of ram");
        }

        // 计算出线程所需的内存大小，然后减少相应的内存大小
        // Subtract some ram to account for dynamic allocation through the code
        uint64_t thread_memory = num_threads * (2 * (stripe_size + 5000)) *
                                 EntrySizes::GetMaxEntrySize(k, 4, true) / (1024 * 1024);

        // 最小内存，将输入内存大小 * 0.05后和50相比，取出最小值，然后加上5，再加上线程需要的内存大小，就是需要的最小内存大小
        uint64_t sub_mbytes = (5 + (int)std::min(buf_megabytes * 0.05, (double)50) + thread_memory);
        // 输入内存大小和最小内存大小比较，如果输入内存大小过小，抛异常提示应当至少设置的内存大小
        if (sub_mbytes > buf_megabytes) {
            throw InsufficientMemoryException(
                "Please provide more memory. At least " + std::to_string(sub_mbytes));
        }

        uint64_t memory_size = ((uint64_t)(buf_megabytes - sub_mbytes)) * 1024 * 1024;
        
        // 计算最大列表大小
        double max_table_size = 0;
        for (size_t i = 1; i <= 7; i++) {
            double memory_i = 1.3 * ((uint64_t)1 << k) * EntrySizes::GetMaxEntrySize(k, i, true);
            if (memory_i > max_table_size)
                max_table_size = memory_i;
        }

        // 计算桶的数量
        if (num_buckets_input != 0) {
            num_buckets = Util::RoundPow2(num_buckets_input);
        } else {
            num_buckets = 2 * Util::RoundPow2(ceil(
                                  ((double)max_table_size) / (memory_size * kMemSortProportion)));
        }

        // 对桶的数量进行取值范围判断，过小或者过大都会抛出异常提醒。最小16，最大128。
        if (num_buckets < kMinBuckets) {
            if (num_buckets_input != 0) {
                throw InvalidValueException("Minimum buckets is " + std::to_string(kMinBuckets));
            }
            num_buckets = kMinBuckets;
        } else if (num_buckets > kMaxBuckets) {
            if (num_buckets_input != 0) {
                throw InvalidValueException("Maximum buckets is " + std::to_string(kMaxBuckets));
            }
            double required_mem =
                (max_table_size / kMaxBuckets) / kMemSortProportion / (1024 * 1024) + sub_mbytes;
            throw InsufficientMemoryException(
                "Do not have enough memory. Need " + std::to_string(required_mem) + " MiB");
        }
        uint32_t log_num_buckets = log2(num_buckets);
        assert(log2(num_buckets) == ceil(log2(num_buckets)));

        // 判断条纹深度是否设置正确
        if (max_table_size / num_buckets < stripe_size * 30) {
            throw InvalidValueException("Stripe size too large");
        }


#if defined(_WIN32) || defined(__x86_64__)
        // 在Windows环境下判断是否支持位域绘图，Why?
        if (!nobitfield && !Util::HavePopcnt()) {
            throw InvalidValueException("Bitfield plotting not supported by CPU");
        }
#endif /* defined(_WIN32) || defined(__x86_64__) */

        // 输出创建Plot的相关参数，实例：
        // Starting plotting progress into temporary dirs: E:\P and E:\P
        // ID: 072e5997602e5796b8966ac1e75044d2e81b2897ebab455de18211db12d79a23
        // Plot size is: 32
        // Buffer size is: 3390MiB
        // Using 128 buckets
        // Using 2 threads of stripe size 65536
        std::cout << std::endl
                  << "Starting plotting progress into temporary dirs: " << tmp_dirname << " and "
                  << tmp2_dirname << std::endl;
        std::cout << "ID: " << Util::HexStr(id, id_len) << std::endl;
        std::cout << "Plot size is: " << static_cast<int>(k) << std::endl;
        std::cout << "Buffer size is: " << buf_megabytes << "MiB" << std::endl;
        std::cout << "Using " << num_buckets << " buckets" << std::endl;
        std::cout << "Using " << (int)num_threads << " threads of stripe size " << stripe_size
                  << std::endl;

        // 开始准备Plot绘图所用到的所有文件名：排序文件、表1-7文件、备用临时文件、最终文件临时储存文件，最终文件

        // 跨平台方式连接路径， gulrak库，Why?
        // Cross platform way to concatenate paths, gulrak library.
        std::vector<fs::path> tmp_1_filenames = std::vector<fs::path>();

        // 表0文件将用于对磁盘空间进行排序，表1-7储存在自己的文件中
        // The table0 file will be used for sort on disk spare. tables 1-7 are stored in their own
        // file.
        tmp_1_filenames.push_back(fs::path(tmp_dirname) / fs::path(filename + ".sort.tmp"));
        for (size_t i = 1; i <= 7; i++) {
            tmp_1_filenames.push_back(
                fs::path(tmp_dirname) / fs::path(filename + ".table" + std::to_string(i) + ".tmp"));
        }
        fs::path tmp_2_filename = fs::path(tmp2_dirname) / fs::path(filename + ".2.tmp");
        fs::path final_2_filename = fs::path(final_dirname) / fs::path(filename + ".2.tmp");
        fs::path final_filename = fs::path(final_dirname) / fs::path(filename);

        // 检查相关目录是否存在，不存在抛异常
        // Check if the paths exist
        if (!fs::exists(tmp_dirname)) {
            throw InvalidValueException("Temp directory " + tmp_dirname + " does not exist");
        }

        if (!fs::exists(tmp2_dirname)) {
            throw InvalidValueException("Temp2 directory " + tmp2_dirname + " does not exist");
        }

        if (!fs::exists(final_dirname)) {
            throw InvalidValueException("Final directory " + final_dirname + " does not exist");
        }

        // 判断相关文件是否已经存在，存在则进行删除
        for (fs::path& p : tmp_1_filenames) {
            fs::remove(p);
        }
        fs::remove(tmp_2_filename);
        fs::remove(final_filename);

        // 取消cin和count与stdio的同步，进行提速
        std::ios_base::sync_with_stdio(false);
        std::ostream* prevstr = std::cin.tie(NULL);

        {
            // 文件操作部分
            // Scope for FileDisk
            std::vector<FileDisk> tmp_1_disks;
            for (auto const& fname : tmp_1_filenames)
                tmp_1_disks.emplace_back(fname);

            FileDisk tmp2_disk(tmp_2_filename);

            assert(id_len == kIdLen);

            // 开始阶段 1/4：前向传播到tmp文件中
            // Starting phase 1/4: Forward Propagation into tmp files... Sun May  2 10:56:36 2021
            std::cout << std::endl
                      << "Starting phase 1/4: Forward Propagation into tmp files... "
                      << Timer::GetNow();

            Timer p1;
            Timer all_phases;
            std::vector<uint64_t> table_sizes = RunPhase1(
                tmp_1_disks,
                k,
                id,
                tmp_dirname,
                filename,
                memory_size,
                num_buckets,
                log_num_buckets,
                stripe_size,
                num_threads,
                !nobitfield,
                show_progress);
            p1.PrintElapsed("Time for phase 1 =");  // Time for phase 1 = 15890.430 seconds. CPU (158.890%) Sun May  2 15:21:26 2021

            uint64_t finalsize=0;

            if(nobitfield)
            {
                // Memory to be used for sorting and buffers
                std::unique_ptr<uint8_t[]> memory(new uint8_t[memory_size + 7]);

                std::cout << std::endl
                      << "Starting phase 2/4: Backpropagation without bitfield into tmp files... "
                      << Timer::GetNow();

                Timer p2;
                std::vector<uint64_t> backprop_table_sizes = b17RunPhase2(
                    memory.get(),
                    tmp_1_disks,
                    table_sizes,
                    k,
                    id,
                    tmp_dirname,
                    filename,
                    memory_size,
                    num_buckets,
                    log_num_buckets,
                    show_progress);
                p2.PrintElapsed("Time for phase 2 =");

                // Now we open a new file, where the final contents of the plot will be stored.
                uint32_t header_size = WriteHeader(tmp2_disk, k, id, memo, memo_len);

                std::cout << std::endl
                      << "Starting phase 3/4: Compression without bitfield from tmp files into " << tmp_2_filename
                      << " ... " << Timer::GetNow();
                Timer p3;
                b17Phase3Results res = b17RunPhase3(
                    memory.get(),
                    k,
                    tmp2_disk,
                    tmp_1_disks,
                    backprop_table_sizes,
                    id,
                    tmp_dirname,
                    filename,
                    header_size,
                    memory_size,
                    num_buckets,
                    log_num_buckets,
                    show_progress);
                p3.PrintElapsed("Time for phase 3 =");

                std::cout << std::endl
                      << "Starting phase 4/4: Write Checkpoint tables into " << tmp_2_filename
                      << " ... " << Timer::GetNow();
                Timer p4;
                b17RunPhase4(k, k + 1, tmp2_disk, res, show_progress, 16);
                p4.PrintElapsed("Time for phase 4 =");
                finalsize = res.final_table_begin_pointers[11];
            }
            else {
                std::cout << std::endl
                      << "Starting phase 2/4: Backpropagation into tmp files... "
                      << Timer::GetNow();

                Timer p2;
                Phase2Results res2 = RunPhase2(
                    tmp_1_disks,
                    table_sizes,
                    k,
                    id,
                    tmp_dirname,
                    filename,
                    memory_size,
                    num_buckets,
                    log_num_buckets,
                    show_progress);
                p2.PrintElapsed("Time for phase 2 =");

                // Now we open a new file, where the final contents of the plot will be stored.
                uint32_t header_size = WriteHeader(tmp2_disk, k, id, memo, memo_len);

                std::cout << std::endl
                      << "Starting phase 3/4: Compression from tmp files into " << tmp_2_filename
                      << " ... " << Timer::GetNow();
                Timer p3;
                Phase3Results res = RunPhase3(
                    k,
                    tmp2_disk,
                    std::move(res2),
                    id,
                    tmp_dirname,
                    filename,
                    header_size,
                    memory_size,
                    num_buckets,
                    log_num_buckets,
                    show_progress);
                p3.PrintElapsed("Time for phase 3 =");

                std::cout << std::endl
                      << "Starting phase 4/4: Write Checkpoint tables into " << tmp_2_filename
                      << " ... " << Timer::GetNow();
                Timer p4;
                RunPhase4(k, k + 1, tmp2_disk, res, show_progress, 16);
                p4.PrintElapsed("Time for phase 4 =");
                finalsize = res.final_table_begin_pointers[11];
            }

            // The total number of bytes used for sort is saved to table_sizes[0]. All other
            // elements in table_sizes represent the total number of entries written by the end of
            // phase 1 (which should be the highest total working space time). Note that the max
            // sort on disk space does not happen at the exact same time as max table sizes, so this
            // estimate is conservative (high).
            uint64_t total_working_space = table_sizes[0];
            for (size_t i = 1; i <= 7; i++) {
                total_working_space += table_sizes[i] * EntrySizes::GetMaxEntrySize(k, i, false);
            }
            std::cout << "Approximate working space used (without final file): "
                      << static_cast<double>(total_working_space) / (1024 * 1024 * 1024) << " GiB"
                      << std::endl;

            std::cout << "Final File size: "
                      << static_cast<double>(finalsize) /
                             (1024 * 1024 * 1024)
                      << " GiB" << std::endl;
            all_phases.PrintElapsed("Total time =");
        }

        std::cin.tie(prevstr);
        std::ios_base::sync_with_stdio(true);

        for (fs::path p : tmp_1_filenames) {
            fs::remove(p);
        }

        bool bCopied = false;
        bool bRenamed = false;
        Timer copy;
        do {
            std::error_code ec;
            if (tmp_2_filename.parent_path() == final_filename.parent_path()) {
                fs::rename(tmp_2_filename, final_filename, ec);
                if (ec.value() != 0) {
                    std::cout << "Could not rename " << tmp_2_filename << " to " << final_filename
                              << ". Error " << ec.message() << ". Retrying in five minutes."
                              << std::endl;
                } else {
                    bRenamed = true;
                    std::cout << "Renamed final file from " << tmp_2_filename << " to "
                              << final_filename << std::endl;
                }
            } else {
                if (!bCopied) {
                    fs::copy(
                        tmp_2_filename, final_2_filename, fs::copy_options::overwrite_existing, ec);
                    if (ec.value() != 0) {
                        std::cout << "Could not copy " << tmp_2_filename << " to "
                                  << final_2_filename << ". Error " << ec.message()
                                  << ". Retrying in five minutes." << std::endl;
                    } else {
                        std::cout << "Copied final file from " << tmp_2_filename << " to "
                                  << final_2_filename << std::endl;
                        copy.PrintElapsed("Copy time =");
                        bCopied = true;

                        bool removed_2 = fs::remove(tmp_2_filename);
                        std::cout << "Removed temp2 file " << tmp_2_filename << "? " << removed_2
                                  << std::endl;
                    }
                }
                if (bCopied && (!bRenamed)) {
                    fs::rename(final_2_filename, final_filename, ec);
                    if (ec.value() != 0) {
                        std::cout << "Could not rename " << tmp_2_filename << " to "
                                  << final_filename << ". Error " << ec.message()
                                  << ". Retrying in five minutes." << std::endl;
                    } else {
                        std::cout << "Renamed final file from " << final_2_filename << " to "
                                  << final_filename << std::endl;
                        bRenamed = true;
                    }
                }
            }

            if (!bRenamed) {
#ifdef _WIN32
                Sleep(5 * 60000);
#else
                sleep(5 * 60);
#endif
            }
        } while (!bRenamed);
    }

private:
    // Writes the plot file header to a file
    uint32_t WriteHeader(
        FileDisk& plot_Disk,
        uint8_t k,
        const uint8_t* id,
        const uint8_t* memo,
        uint32_t memo_len)
    {
        // 19 bytes  - "Proof of Space Plot" (utf-8)
        // 32 bytes  - unique plot id
        // 1 byte    - k
        // 2 bytes   - format description length
        // x bytes   - format description
        // 2 bytes   - memo length
        // x bytes   - memo

        std::string header_text = "Proof of Space Plot";
        uint64_t write_pos = 0;
        plot_Disk.Write(write_pos, (uint8_t*)header_text.data(), header_text.size());
        write_pos += header_text.size();
        plot_Disk.Write(write_pos, (id), kIdLen);
        write_pos += kIdLen;

        uint8_t k_buffer[1];
        k_buffer[0] = k;
        plot_Disk.Write(write_pos, (k_buffer), 1);
        write_pos += 1;

        uint8_t size_buffer[2];
        Util::IntToTwoBytes(size_buffer, kFormatDescription.size());
        plot_Disk.Write(write_pos, (size_buffer), 2);
        write_pos += 2;
        plot_Disk.Write(write_pos, (uint8_t*)kFormatDescription.data(), kFormatDescription.size());
        write_pos += kFormatDescription.size();

        Util::IntToTwoBytes(size_buffer, memo_len);
        plot_Disk.Write(write_pos, (size_buffer), 2);
        write_pos += 2;
        plot_Disk.Write(write_pos, (memo), memo_len);
        write_pos += memo_len;

        uint8_t pointers[10 * 8];
        memset(pointers, 0, 10 * 8);
        plot_Disk.Write(write_pos, (pointers), 10 * 8);
        write_pos += 10 * 8;

        uint32_t bytes_written =
            header_text.size() + kIdLen + 1 + 2 + kFormatDescription.size() + 2 + memo_len + 10 * 8;
        std::cout << "Wrote: " << bytes_written << std::endl;
        return bytes_written;
    }
};

#endif  // SRC_CPP_PLOTTER_DISK_HPP_
