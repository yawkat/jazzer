// Copyright 2021 Code Intelligence GmbH
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "coverage_tracker.h"

#include <jni.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

extern "C" void __sanitizer_cov_8bit_counters_init(uint8_t *start,
                                                   uint8_t *end);
extern "C" void __sanitizer_cov_pcs_init(const uintptr_t *pcs_beg,
                                         const uintptr_t *pcs_end);
extern "C" size_t __sanitizer_cov_get_observed_pcs(uintptr_t **pc_entries);

constexpr auto kCoverageRecorderClass =
    "com/code_intelligence/jazzer/instrumentor/CoverageRecorder";

namespace {
void AssertNoException(JNIEnv &env) {
  if (env.ExceptionCheck()) {
    env.ExceptionDescribe();
    throw std::runtime_error(
        "Java exception occurred in CoverageTracker JNI code");
  }
}
}  // namespace

namespace jazzer {

uint8_t *CoverageTracker::counters_ = nullptr;
uint32_t *CoverageTracker::fake_instructions_ = nullptr;
PCTableEntry *CoverageTracker::pc_entries_ = nullptr;

void CoverageTracker::Initialize(JNIEnv &env, jlong counters) {
  if (counters_ != nullptr) {
    throw std::runtime_error(
        "CoverageTracker::Initialize must not be called more than once");
  }
  counters_ = reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(counters));
}

void CoverageTracker::RegisterNewCounters(JNIEnv &env, jint old_num_counters,
                                          jint new_num_counters) {
  if (counters_ == nullptr) {
    throw std::runtime_error(
        "CoverageTracker::Initialize should have been called first");
  }
  if (new_num_counters < old_num_counters) {
    throw std::runtime_error(
        "new_num_counters must not be smaller than old_num_counters");
  }
  if (new_num_counters == old_num_counters) {
    return;
  }
  std::size_t diff_num_counters = new_num_counters - old_num_counters;
  // libFuzzer requires an array containing the instruction addresses associated
  // with the coverage counters registered above. Given that we are
  // instrumenting Java code, we need to synthesize addresses that are known not
  // to conflict with any valid instruction address in native code. Just like
  // atheris we ensure there are no collisions by using the addresses of an
  // allocated buffer. Note: We intentionally never deallocate the allocations
  // made here as they have static lifetime and we can't guarantee they wouldn't
  // be freed before libFuzzer stops using them.
  fake_instructions_ = new uint32_t[diff_num_counters];
  std::fill(fake_instructions_, fake_instructions_ + diff_num_counters, 0);

  // Never deallocated, see above.
  pc_entries_ = new PCTableEntry[diff_num_counters];
  for (std::size_t i = 0; i < diff_num_counters; ++i) {
    pc_entries_[i].PC = reinterpret_cast<uintptr_t>(fake_instructions_ + i);
    // TODO: Label Java PCs corresponding to functions as such.
    pc_entries_[i].PCFlags = 0;
  }
  __sanitizer_cov_8bit_counters_init(counters_ + old_num_counters,
                                     counters_ + new_num_counters);
  __sanitizer_cov_pcs_init((uintptr_t *)(pc_entries_),
                           (uintptr_t *)(pc_entries_ + diff_num_counters));
}

uint8_t *CoverageTracker::GetCoverageCounters() { return counters_; }

void CoverageTracker::RecordInitialCoverage(JNIEnv &env) {
  jclass coverage_recorder = env.FindClass(kCoverageRecorderClass);
  AssertNoException(env);
  jmethodID coverage_recorder_update_covered_ids_with_coverage_map =
      env.GetStaticMethodID(coverage_recorder,
                            "updateCoveredIdsWithCoverageMap", "()V");
  AssertNoException(env);
  env.CallStaticVoidMethod(
      coverage_recorder,
      coverage_recorder_update_covered_ids_with_coverage_map);
  AssertNoException(env);
}

void CoverageTracker::ReplayInitialCoverage(JNIEnv &env) {
  jclass coverage_recorder = env.FindClass(kCoverageRecorderClass);
  AssertNoException(env);
  jmethodID coverage_recorder_update_covered_ids_with_coverage_map =
      env.GetStaticMethodID(coverage_recorder, "replayCoveredIds", "()V");
  AssertNoException(env);
  env.CallStaticVoidMethod(
      coverage_recorder,
      coverage_recorder_update_covered_ids_with_coverage_map);
  AssertNoException(env);
}

std::string CoverageTracker::ComputeCoverage(JNIEnv &env) {
  uintptr_t *covered_pcs;
  size_t num_covered_pcs = __sanitizer_cov_get_observed_pcs(&covered_pcs);
  std::vector<jint> covered_edge_ids{};
  covered_edge_ids.reserve(num_covered_pcs);
  const uintptr_t first_pc = pc_entries_[0].PC;
  std::for_each(covered_pcs, covered_pcs + num_covered_pcs,
                [&covered_edge_ids, first_pc](const uintptr_t pc) {
                  jint edge_id =
                      (pc - first_pc) / sizeof(fake_instructions_[0]);
                  covered_edge_ids.push_back(edge_id);
                });
  delete[] covered_pcs;

  jclass coverage_recorder = env.FindClass(kCoverageRecorderClass);
  AssertNoException(env);
  jmethodID coverage_recorder_compute_file_coverage = env.GetStaticMethodID(
      coverage_recorder, "computeFileCoverage", "([I)Ljava/lang/String;");
  AssertNoException(env);
  jintArray covered_edge_ids_jni = env.NewIntArray(num_covered_pcs);
  AssertNoException(env);
  env.SetIntArrayRegion(covered_edge_ids_jni, 0, num_covered_pcs,
                        covered_edge_ids.data());
  AssertNoException(env);
  auto file_coverage_jni = (jstring)(env.CallStaticObjectMethod(
      coverage_recorder, coverage_recorder_compute_file_coverage,
      covered_edge_ids_jni));
  AssertNoException(env);
  auto file_coverage_cstr = env.GetStringUTFChars(file_coverage_jni, nullptr);
  AssertNoException(env);
  std::string file_coverage(file_coverage_cstr);
  env.ReleaseStringUTFChars(file_coverage_jni, file_coverage_cstr);
  AssertNoException(env);
  return file_coverage;
}
}  // namespace jazzer

extern "C" {
JNIEXPORT void JNICALL
Java_com_code_1intelligence_jazzer_runtime_CoverageMap_initialize(
    JNIEnv &env, jclass cls, jlong counters) {
  ::jazzer::CoverageTracker::Initialize(env, counters);
}

JNIEXPORT void JNICALL
Java_com_code_1intelligence_jazzer_runtime_CoverageMap_registerNewCounters(
    JNIEnv &env, jclass cls, jint old_num_counters, jint new_num_counters) {
  ::jazzer::CoverageTracker::RegisterNewCounters(env, old_num_counters,
                                                 new_num_counters);
}
}
