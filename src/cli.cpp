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

#include <ctime>
#include <set>

#include "cxxopts.hpp"
#include "../lib/include/picosha2.hpp"
#include "plotter_disk.hpp"
#include "prover_disk.hpp"
#include "verifier.hpp"

using std::string;
using std::vector;
using std::endl;
using std::cout;

void HexToBytes(const string &hex, uint8_t *result)
{
    for (uint32_t i = 0; i < hex.length(); i += 2) {
        string byteString = hex.substr(i, 2);
        uint8_t byte = (uint8_t)strtol(byteString.c_str(), NULL, 16);
        result[i / 2] = byte;
    }
}

vector<unsigned char> intToBytes(uint32_t paramInt, uint32_t numBytes)
{
    vector<unsigned char> arrayOfByte(numBytes, 0);
    for (uint32_t i = 0; paramInt > 0; i++) {
        arrayOfByte[numBytes - i - 1] = paramInt & 0xff;
        paramInt >>= 8;
    }
    return arrayOfByte;
}

string Strip0x(const string &hex)
{
    if (hex.size() > 1 && (hex.substr(0, 2) == "0x" || hex.substr(0, 2) == "0X")) {
        return hex.substr(2);
    }
    return hex;
}

void HelpAndQuit(cxxopts::Options options)
{
    cout << options.help({""}) << endl;
    cout << "./ProofOfSpace create" << endl;
    cout << "./ProofOfSpace prove <challenge>" << endl;
    cout << "./ProofOfSpace verify <proof> <challenge>" << endl;
    cout << "./ProofOfSpace check" << endl;
    exit(0);
}

// 主函数
int main(int argc, char *argv[]) try {

    // 输出此应用程序的使用说明，空间证明，用于绘制、生成和验证空间证明。
    cxxopts::Options options(
        "ProofOfSpace", "Utility for plotting, generating and verifying proofs of space.");
    
    // 输出运行指令，分别有：创建、测试、校验、检查，指令后面按照空格间隔，传递相应的参数
    // 调用实例：
    // ./ProofOfSpace create                        // 创建，.\ProofOfSpace.exe create -k 25 -r 2 -b 4608 -u 64 -t . -2 . -d .
    // ./ProofOfSpace prove <challenge>             // 测试，参数：挑战， ./ProofOfSpace -f "plot.dat" prove 0x48e14b6feedc9d80e0e9338c8f7e5bbdce80d2df2bc3d9e03a06431c4036f6b7
    // ./ProofOfSpace verify <proof> <challenge>    // 校验，参数1：proof文件，参数2：挑战，./ProofOfSpace -k 25 verify <hex proof> <32 byte hex challenge>，未测试，不知道hex proof是什么
    // ./ProofOfSpace check                         // 检查，./ProofOfSpace check -f ./plot.dat 100
    options.positional_help("(create/prove/verify/check) param1 param2 ")
        .show_positional_help();





    // 默认参数
    // Default values
    uint8_t k = 20;                 // K的大小
    uint32_t num_buckets = 0;       // 桶的数量
    uint32_t num_stripes = 0;       // 条带深度
    uint8_t num_threads = 0;        // 线程数量
    string filename = "plot.dat";   // Plots文件的后缀名
    string tempdir = ".";           // 临时文件存放路径，默认为当前路径下
    string tempdir2 = ".";          // 备用临时文件存放路径，默认为当前路径下
    string finaldir = ".";          // 最终文件存放路径，默认为当前路径下
    string operation = "help";      // 运行指令
    string memo = "b523cd9d58972af56ba6d5d61ccdf77e76894bafa5df3785055334e98e9b7dcacf21d41c491d2d876767df304e2742ae939df12309be853da848961b2089f9c3620622a1f2e49fd0fa74f228a006367000e58d3ded9df8004de5c54acada43805d18adec05f526be9cfc7aba062ac22608a05495c153b54bae4e46002f5295c0";     // 脑密码 // 0102030405
    string id = "fa1e527bc1d8070876ddb40b4cd50c1d8dacf4d361e71fb61fd613b65c64b506"; // Id  // 022fb42c08c12de3a6af053880199806532e79515f94e83461612101f9412f9e
    bool nobitfield = false;        // 关闭bitfield(、位字段、字节牧场)
    bool show_progress = false;     // 显示进度
    uint32_t buffmegabytes = 0;     // 基础什么什么字节数

    options.allow_unrecognised_options().add_options()(
        // k，大小，Plot文件的大小
        "k, size", "Plot size", cxxopts::value<uint8_t>(k))(
        // r, 线程，线程数量
        "r, threads", "Number of threads", cxxopts::value<uint8_t>(num_threads))(
        // u, 桶，桶的大小
        "u, buckets", "Number of buckets", cxxopts::value<uint32_t>(num_buckets))(
        // s, 条纹，条纹的大小
        "s, stripes", "Size of stripes", cxxopts::value<uint32_t>(num_stripes))(
        // t，临时目录，临时文件存放路径
        "t, tempdir", "Temporary directory", cxxopts::value<string>(tempdir))(
        // 2，备用临时目录，备用临时文件存放路径
        "2, tempdir2", "Second Temporary directory", cxxopts::value<string>(tempdir2))(
        // d，最终目录，最终文件存放目录
        "d, finaldir", "Final directory", cxxopts::value<string>(finaldir))(
        // f，文件名，最终文件的文件名
        "f, file", "Filename", cxxopts::value<string>(filename))(
        // m，填写到Plot文件中的备忘录
        "m, memo", "Memo to insert into the plot", cxxopts::value<string>(memo))(
        // i, id，用于绘图的32字节的唯一种子
        "i, id", "Unique 32-byte seed for the plot", cxxopts::value<string>(id))(
        // e, 关闭bitfield，关闭bitfield，关闭
        "e, nobitfield", "Disable bitfield", cxxopts::value<bool>(nobitfield))(
        // b, 用户排序和绘图的缓冲区大小，也就是内存大小
        "b, buffer", "Megabytes to be used as buffer for sorting and plotting",
        cxxopts::value<uint32_t>(buffmegabytes))(
        // p, 进度，在绘图时显示进度百分比
        "p, progress", "Display progress percentage during plotting",
        cxxopts::value<bool>(show_progress))(
        // help, 输出帮助信息
        "help", "Print help");

    auto result = options.parse(argc, argv);

    if (result.count("help") || argc < 2) {
        HelpAndQuit(options);
    }
    operation = argv[1];
    
    
    // 增加调试信息
    std::cout << "Semaphore Test" << std::endl;
         
    std::cout << "operation: " << operation << std::endl;

    if (operation == "help") {
        HelpAndQuit(options);
    } else if (operation == "create") {
        cout << "Generating plot for k=" << static_cast<int>(k) << " filename=" << filename
             << " id=" << id << endl
             << endl;
        id = Strip0x(id);
        if (id.size() != 64) {
            cout << "Invalid ID, should be 32 bytes (hex)" << endl;
            exit(1);
        }
        memo = Strip0x(memo);
        if (memo.size() % 2 != 0) {
            cout << "Invalid memo, should be only whole bytes (hex)" << endl;
            exit(1);
        }
        std::vector<uint8_t> memo_bytes(memo.size() / 2);
        std::array<uint8_t, 32> id_bytes;

        HexToBytes(memo, memo_bytes.data());
        HexToBytes(id, id_bytes.data());

        DiskPlotter plotter = DiskPlotter();
        plotter.CreatePlotDisk(
                tempdir,
                tempdir2,
                finaldir,
                filename,
                k,
                memo_bytes.data(),
                memo_bytes.size(),
                id_bytes.data(),
                id_bytes.size(),
                buffmegabytes,
                num_buckets,
                num_stripes,
                num_threads,
                nobitfield,
                show_progress);
    } else if (operation == "prove") {
        if (argc < 3) {
            HelpAndQuit(options);
        }
        cout << "Proving using filename=" << filename << " challenge=" << argv[2] << endl
             << endl;
        string challenge = Strip0x(argv[2]);
        if (challenge.size() != 64) {
            cout << "Invalid challenge, should be 32 bytes" << endl;
            exit(1);
        }
        uint8_t challenge_bytes[32];
        HexToBytes(challenge, challenge_bytes);

        DiskProver prover(filename);
        try {
            vector<LargeBits> qualities = prover.GetQualitiesForChallenge(challenge_bytes);
            for (uint32_t i = 0; i < qualities.size(); i++) {
                k = prover.GetSize();
                uint8_t *proof_data = new uint8_t[8 * k];
                LargeBits proof = prover.GetFullProof(challenge_bytes, i);
                proof.ToBytes(proof_data);
                cout << "Proof: 0x" << Util::HexStr(proof_data, k * 8) << endl;
                delete[] proof_data;
            }
            if (qualities.empty()) {
                cout << "No proofs found." << endl;
                exit(1);
            }
        } catch (const std::exception& ex) {
            std::cout << "Error proving. " << ex.what() << std::endl;
            exit(1);
        } catch (...) {
            std::cout << "Error proving. " << std::endl;
            exit(1);
        }
    } else if (operation == "verify") {
        if (argc < 4) {
            HelpAndQuit(options);
        }
        Verifier verifier = Verifier();

        id = Strip0x(id);
        string proof = Strip0x(argv[2]);
        string challenge = Strip0x(argv[3]);
        if (id.size() != 64) {
            cout << "Invalid ID, should be 32 bytes" << endl;
            exit(1);
        }
        if (challenge.size() != 64) {
            cout << "Invalid challenge, should be 32 bytes" << endl;
            exit(1);
        }
        if (proof.size() % 16) {
            cout << "Invalid proof, should be a multiple of 8 bytes" << endl;
            exit(1);
        }
        k = proof.size() / 16;
        cout << "Verifying proof=" << argv[2] << " for challenge=" << argv[3]
             << " and k=" << static_cast<int>(k) << endl
             << endl;
        uint8_t id_bytes[32];
        uint8_t challenge_bytes[32];
        uint8_t *proof_bytes = new uint8_t[proof.size() / 2];
        HexToBytes(id, id_bytes);
        HexToBytes(challenge, challenge_bytes);
        HexToBytes(proof, proof_bytes);

        LargeBits quality =
            verifier.ValidateProof(id_bytes, k, challenge_bytes, proof_bytes, k * 8);
        if (quality.GetSize() == 256) {
            cout << "Proof verification suceeded. Quality: " << quality << endl;
        } else {
            cout << "Proof verification failed." << endl;
            exit(1);
        }
        delete[] proof_bytes;
    } else if (operation == "check") {
        uint32_t iterations = 1000;
        if (argc == 3) {
            iterations = std::stoi(argv[2]);
        }

        DiskProver prover(filename);
        Verifier verifier = Verifier();

        uint32_t success = 0;
        uint8_t id_bytes[32];
        prover.GetId(id_bytes);
        k = prover.GetSize();

        for (uint32_t num = 0; num < iterations; num++) {
            vector<unsigned char> hash_input = intToBytes(num, 4);
            hash_input.insert(hash_input.end(), &id_bytes[0], &id_bytes[32]);

            vector<unsigned char> hash(picosha2::k_digest_size);
            picosha2::hash256(hash_input.begin(), hash_input.end(), hash.begin(), hash.end());

            try {
                vector<LargeBits> qualities = prover.GetQualitiesForChallenge(hash.data());

                for (uint32_t i = 0; i < qualities.size(); i++) {
                    LargeBits proof = prover.GetFullProof(hash.data(), i);
                    uint8_t *proof_data = new uint8_t[proof.GetSize() / 8];
                    proof.ToBytes(proof_data);
                    cout << "i: " << num << std::endl;
                    cout << "challenge: 0x" << Util::HexStr(hash.data(), 256 / 8) << endl;
                    cout << "proof: 0x" << Util::HexStr(proof_data, k * 8) << endl;
                    LargeBits quality =
                        verifier.ValidateProof(id_bytes, k, hash.data(), proof_data, k * 8);
                    if (quality.GetSize() == 256 && quality == qualities[i]) {
                        cout << "quality: " << quality << endl;
                        cout << "Proof verification suceeded. k = " << static_cast<int>(k) << endl;
                        success++;
                    } else {
                        cout << "Proof verification failed." << endl;
                    }
                    delete[] proof_data;
                }
            } catch (const std::exception& error) {
                cout << "Threw: " << error.what() << endl;
                continue;
            }
        }
        std::cout << "Total success: " << success << "/" << iterations << ", "
                  << (success * 100 / static_cast<double>(iterations)) << "%." << std::endl;
        if (show_progress) { progress(4, 1, 1); }
    } else {
        cout << "Invalid operation. Use create/prove/verify/check" << endl;
    }
    return 0;
} catch (const cxxopts::OptionException &e) {
    cout << "error parsing options: " << e.what() << endl;
    return 1;
} catch (const std::exception &e) {
    std::cerr << "Caught exception: " << e.what() << endl;
    throw e;
}
