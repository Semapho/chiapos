#ifndef PYTHON_BINDINGS_PYTHON_BINDINGS_HPP_
#define PYTHON_BINDINGS_PYTHON_BINDINGS_HPP_

#if __has_include(<optional>)

#include <optional>
namespace stdx {
using std::optional;
}

#elif __has_include(<experimental/optional>)

#include <experimental/optional>
namespace stdx {
using std::experimental::optional;
}

#else
#error "an implementation of optional is required!"
#endif

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "../src/plotter_disk.hpp"
#include "../src/prover_disk.hpp"
#include "../src/verifier.hpp"


/*
 * 此源码为封装将C++方法通过PyBind11提供给Python调用的接口
 */

namespace py = pybind11;

PYBIND11_MODULE(chiapos, m)
{
    m.doc() = "Chia Proof of Space";

    // 类：DiskPlotter，创建Plot的类
    py::class_<DiskPlotter>(m, "DiskPlotter")
        .def(py::init<>())

        // 接口：create_plot_disk，在硬盘上创建Plot的方法
        .def(
            "create_plot_disk",
            [](
                // 参数，
               DiskPlotter &dp,
               const std::string tmp_dir,
               const std::string tmp2_dir,
               const std::string final_dir,
               const std::string filename,
               uint8_t k,
               const py::bytes &memo,
               const py::bytes &id,
               uint32_t buffmegabytes,
               uint32_t num_buckets,
               uint32_t stripe_size,
               uint8_t num_threads,
               bool nobitfield) {
                std::string memo_str(memo);
                const uint8_t *memo_ptr = reinterpret_cast<const uint8_t *>(memo_str.data());
                std::string id_str(id);
                const uint8_t *id_ptr = reinterpret_cast<const uint8_t *>(id_str.data());
                try {
                    // 调用方法，在硬盘上创建Plot文件
                    dp.CreatePlotDisk(tmp_dir,
                                      tmp2_dir,
                                      final_dir,
                                      filename,
                                      k,
                                      memo_ptr,
                                      len(memo),
                                      id_ptr,
                                      len(id),
                                      buffmegabytes,
                                      num_buckets,
                                      stripe_size,
                                      num_threads,
                                      nobitfield);
                } catch (const std::exception &e) {
                    std::cout << "Caught plotting error: " << e.what() << std::endl;
                    throw e;
                }
            });

    py::class_<DiskProver>(m, "DiskProver")
        .def(py::init<const std::string &>())
        .def(
            "get_memo",
            [](DiskProver &dp) {
                uint8_t *memo = new uint8_t[dp.GetMemoSize()];
                dp.GetMemo(memo);
                py::bytes ret = py::bytes(reinterpret_cast<char *>(memo), dp.GetMemoSize());
                delete[] memo;
                return ret;
            })
        .def(
            "get_id",
            [](DiskProver &dp) {
                uint8_t *id = new uint8_t[kIdLen];
                dp.GetId(id);
                py::bytes ret = py::bytes(reinterpret_cast<char *>(id), kIdLen);
                delete[] id;
                return ret;
            })
        .def("get_size", [](DiskProver &dp) { return dp.GetSize(); })
        .def("get_filename", [](DiskProver &dp) { return dp.GetFilename(); })
        .def(
            "get_qualities_for_challenge",
            [](DiskProver &dp, const py::bytes &challenge) {
                if (len(challenge) != 32) {
                    throw std::invalid_argument("Challenge must be exactly 32 bytes");
                }
                std::string challenge_str(challenge);
                const uint8_t *challenge_ptr =
                    reinterpret_cast<const uint8_t *>(challenge_str.data());
                py::gil_scoped_release release;
                std::vector<LargeBits> qualities = dp.GetQualitiesForChallenge(challenge_ptr);
                py::gil_scoped_acquire acquire;
                std::vector<py::bytes> ret;
                uint8_t *quality_buf = new uint8_t[32];
                for (LargeBits quality : qualities) {
                    quality.ToBytes(quality_buf);
                    py::bytes quality_py = py::bytes(reinterpret_cast<char *>(quality_buf), 32);
                    ret.push_back(quality_py);
                }
                delete[] quality_buf;
                return ret;
            })
        .def("get_full_proof", [](DiskProver &dp, const py::bytes &challenge, uint32_t index) {
            std::string challenge_str(challenge);
            const uint8_t *challenge_ptr = reinterpret_cast<const uint8_t *>(challenge_str.data());
            py::gil_scoped_release release;
            LargeBits proof = dp.GetFullProof(challenge_ptr, index);
            py::gil_scoped_acquire acquire;
            uint8_t *proof_buf = new uint8_t[Util::ByteAlign(64 * dp.GetSize()) / 8];
            proof.ToBytes(proof_buf);
            py::bytes ret = py::bytes(
                reinterpret_cast<char *>(proof_buf), Util::ByteAlign(64 * dp.GetSize()) / 8);
            delete[] proof_buf;
            return ret;
        });

    py::class_<Verifier>(m, "Verifier")
        .def(py::init<>())
        .def(
            "validate_proof",
            [](Verifier &v,
               const py::bytes &seed,
               uint8_t k,
               const py::bytes &challenge,
               const py::bytes &proof) {
                std::string seed_str(seed);
                const uint8_t *seed_ptr = reinterpret_cast<const uint8_t *>(seed_str.data());

                std::string challenge_str(challenge);
                const uint8_t *challenge_ptr =
                    reinterpret_cast<const uint8_t *>(challenge_str.data());

                std::string proof_str(proof);
                const uint8_t *proof_ptr = reinterpret_cast<const uint8_t *>(proof_str.data());

                LargeBits quality =
                    v.ValidateProof(seed_ptr, k, challenge_ptr, proof_ptr, len(proof));
                if (quality.GetSize() == 0) {
                    return stdx::optional<py::bytes>();
                }
                uint8_t *quality_buf = new uint8_t[32];
                quality.ToBytes(quality_buf);
                py::bytes quality_py = py::bytes(reinterpret_cast<char *>(quality_buf), 32);
                delete[] quality_buf;
                return stdx::optional<py::bytes>(quality_py);
            });
}

#endif  // PYTHON_BINDINGS_PYTHON_BINDINGS_HPP_
