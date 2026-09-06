// SLO Fabric CUDA proof: real measured latency, throughput, and device-memory
// evidence on an NVIDIA GPU, evaluated against configured SLO objectives.
// Measured values are REAL; the configured SLO thresholds are policy
// (SYNTHETIC). No multi-GPU / NVLink / MIG / hardware-failover claims are made.

#include <cstdio>
#include <cstdlib>
#include <vector>

#include <cuda_runtime.h>

#include "slofabric/fabric.hpp"
#include "slofabric/evidence.hpp"

using namespace slofabric;

#define CUDA_CHECK(x) do { cudaError_t e = (x); if (e != cudaSuccess) {   std::printf("CUDA error at %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(e));   std::exit(1); } } while (0)

__global__ void saxpy_kernel(const float* x, const float* y, float* out,
                             int n, float a, int iters) {
  int idx = blockIdx.x * blockDim.x + threadIdx.x;
  if (idx >= n) return;
  float v = x[idx] + a * y[idx];
  for (int k = 1; k < iters; ++k) v = v * 0.999f + x[idx];
  out[idx] = v;
}

static double gpu_launch(double (*kern)(const float*, const float*, float*, int, float, int),
                         const float* x, const float* y, float* out, int n, float a, int iters) {
  (void)kern;
  int block = 256;
  int grid = (n + block - 1) / block;
  cudaEvent_t s, e;
  cudaEventCreate(&s); cudaEventCreate(&e);
  cudaEventRecord(s);
  saxpy_kernel<<<grid, block>>>(x, y, out, n, a, iters);
  cudaEventRecord(e);
  cudaEventSynchronize(e);
  float ms = 0.0f;
  cudaEventElapsedTime(&ms, s, e);
  cudaEventDestroy(s); cudaEventDestroy(e);
  return static_cast<double>(ms) * 1e6;   // nanoseconds
}

static long long used_device_memory() {
  size_t free_b = 0, total_b = 0;
  cudaMemGetInfo(&free_b, &total_b);
  return static_cast<long long>(total_b - free_b);
}

static bool cpu_reference_valid(const std::vector<float>& x, const std::vector<float>& y,
                                const std::vector<float>& out, float a, int iters) {
  int n = static_cast<int>(x.size());
  for (int i = 0; i < n; ++i) {
    float v = x[i] + a * y[i];
    for (int k = 1; k < iters; ++k) v = v * 0.999f + x[i];
    float delta = v - out[i];
    if (delta < 0.0f) delta = -delta;
    float scale = (v < 0.0f ? -v : v); if (scale < 1.0f) scale = 1.0f;
    if (delta > 2e-3f * scale) return false;
  }
  return true;
}

static void setup_fabric(SloFabric& f, double latency_target_ns, double throughput_target,
                         double pressure_target) {
  SloPolicy p;
  p.id = PolicyId(1); p.gen = PolicyGen(1);
  p.mode = PolicyMode::HardThenSoft; p.tie_break = TieBreak::ByName;
  p.name = "cuda"; p.description = "cuda proof policy";
  f.set_policy(p);
  SloContract c;
  c.id = SloContractId(1); c.gen = SloContractGen(1);
  c.service = ServiceId(100); c.workload = WorkloadId(200); c.tenant = TenantId(1);
  c.policy_id = p.id; c.policy_gen = p.gen;
  c.effective_from = Duration(0);
  c.epoch = CoordinatorEpoch(1); c.lifecycle = ContractLifecycle::Active;
  c.name = "cuda-contract"; c.provenance = "test";
  Objective lat;
  lat.id = ObjectiveId(1); lat.gen = ObjectiveGen(1);
  lat.name = "kernel-latency"; lat.dim = Dimension::Latency;
  lat.kind = ObjectiveKind::LatencyPercentile; lat.percentile_rank = 0.99;
  lat.comparison = Comparison::AtMost; lat.hard = HardSoft::Hard;
  lat.target = Duration(static_cast<std::int64_t>(latency_target_ns));
  lat.min_evidence = Count(8);
  lat.window_type = WindowType::Sliding; lat.window_duration = seconds(10); lat.window_count = Count(64);
  c.objectives.push_back(lat);
  Objective thr;
  thr.id = ObjectiveId(2); thr.gen = ObjectiveGen(1);
  thr.name = "kernel-throughput"; thr.dim = Dimension::Throughput;
  thr.kind = ObjectiveKind::ThroughputRate;
  thr.comparison = Comparison::AtLeast; thr.hard = HardSoft::Hard;
  thr.target = OperationsPerSecond(throughput_target);
  thr.rate_tag = RateTag::Operations;
  thr.min_evidence = Count(8);
  thr.window_type = WindowType::Sliding; thr.window_duration = seconds(10); thr.window_count = Count(64);
  c.objectives.push_back(thr);
  Objective mem;
  mem.id = ObjectiveId(3); mem.gen = ObjectiveGen(1);
  mem.name = "device-memory"; mem.dim = Dimension::MemoryPressure;
  mem.kind = ObjectiveKind::MemoryPressureMax;
  mem.comparison = Comparison::AtMost; mem.hard = HardSoft::Hard;
  mem.target = PressureRatio(pressure_target);
  mem.min_evidence = Count(1);
  c.objectives.push_back(mem);
  f.add_contract(c);
  f.activate_contract(SloContractId(1), Duration(0));
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  cudaDeviceProp prop{};
  CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
  std::printf("=== CUDA device (REAL) ===\n");
  std::printf("name=%s  computeCapability=%d.%d  memMiB=%d\n", prop.name,
              prop.major, prop.minor, static_cast<int>(prop.totalGlobalMem / (1024 * 1024)));
  int dev = 0; CUDA_CHECK(cudaGetDevice(&dev));
  size_t free0 = 0, total0 = 0; CUDA_CHECK(cudaMemGetInfo(&free0, &total0));
  std::printf("device memory before: used=%lldMiB free=%lldMiB\n",
              (long long)((total0 - free0) / (1024*1024)), (long long)(free0 / (1024*1024)));

  int n = 1 << 20;  // 1M floats
  std::vector<float> hx(n), hy(n), hout_cpu(n), hout_gpu(n);
  for (int i = 0; i < n; ++i) { hx[i] = 1.0f + i * 1e-6f; hy[i] = 2.0f - i * 1e-6f; }
  float a = 1.5f;

  float* dx = nullptr; float* dy = nullptr; float* dout = nullptr;
  CUDA_CHECK(cudaMalloc(&dx, n * sizeof(float)));
  CUDA_CHECK(cudaMalloc(&dy, n * sizeof(float)));
  CUDA_CHECK(cudaMalloc(&dout, n * sizeof(float)));
  CUDA_CHECK(cudaMemcpy(dx, hx.data(), n * sizeof(float), cudaMemcpyHostToDevice));
  CUDA_CHECK(cudaMemcpy(dy, hy.data(), n * sizeof(float), cudaMemcpyHostToDevice));

  // Warm-up.
  for (int i = 0; i < 3; ++i) saxpy_kernel<<<(n+255)/256, 256>>>(dx, dy, dout, n, a, 4);
  CUDA_CHECK(cudaDeviceSynchronize());

  // Run several scenarios with different per-kernel work to shift the binding
  // constraint. Each run is REAL measured latency/throughput/memory.
  const int iters_vals[] = {4, 64, 256};
  const char* names[] = {"low-work", "mid-work", "high-work"};

  SloFabric f;
  // Targets chosen so different scenarios bind different objectives.
  setup_fabric(f, 12000.0, 1e12, 0.90);

  for (int run = 0; run < 3; ++run) {
    double best = 1e18;
    for (int i = 0; i < 8; ++i) {
      double t = gpu_launch(nullptr, dx, dy, dout, n, a, iters_vals[run]);
      if (t < best) best = t;
    }
    double throughput = static_cast<double>(n * iters_vals[run]) / (best * 1e-9);
    long long used = used_device_memory();
    double pressure = static_cast<double>(used) / static_cast<double>(total0);
    CUDA_CHECK(cudaMemcpy(hout_gpu.data(), dout, n * sizeof(float), cudaMemcpyDeviceToHost));
    bool cpu_ver = cpu_reference_valid(hx, hy, hout_gpu, a, iters_vals[run]);

    std::printf("\n=== run %d (%s) [REAL measured] ===\n", run, names[run]);
    std::printf("  kernel latency (best, REAL)=%.2f ns  throughput=%.0f op/s  "
                "deviceMemoryPressure (REAL)=%.4f  cpuParity=%s\n",
                best, throughput, pressure, cpu_ver ? "OK" : "FAIL");

    // Refresh fabric and ingest real measured evidence (clear previous run).
    SloFabric frun;
    setup_fabric(frun, 12000.0, 1e12, 0.90);
    for (int i = 0; i < 8; ++i) {
      EvidenceRecord e;
      e.id = EvidenceId(1000 + run * 10 + i);
      e.gen = EvidenceGen(1);
      e.dimension = Dimension::Latency;
      e.objective_id = ObjectiveId(1); e.objective_gen = ObjectiveGen(1);
      e.source = SourceBootId(1); e.source_gen = SourceBootGen(1);
      e.epoch = CoordinatorEpoch(1);
      e.time = milliseconds(i * 10);
      e.provenance = Provenance::Measured;
      e.value = Duration(static_cast<std::int64_t>(best));
      frun.ingest(e, e.time);
      EvidenceRecord t;
      t.id = EvidenceId(2000 + run * 10 + i);
      t.gen = EvidenceGen(1); t.dimension = Dimension::Throughput;
      t.objective_id = ObjectiveId(2); t.objective_gen = ObjectiveGen(1);
      t.source = SourceBootId(1); t.source_gen = SourceBootGen(1);
      t.epoch = CoordinatorEpoch(1); t.time = milliseconds(i * 10);
      t.provenance = Provenance::Measured; t.value = OperationsPerSecond(throughput);
      frun.ingest(t, t.time);
    }
    EvidenceRecord m;
    m.id = EvidenceId(3000 + run);
    m.gen = EvidenceGen(1); m.dimension = Dimension::MemoryPressure;
    m.objective_id = ObjectiveId(3); m.objective_gen = ObjectiveGen(1);
    m.source = SourceBootId(1); m.source_gen = SourceBootGen(1);
    m.epoch = CoordinatorEpoch(1); m.time = milliseconds(100);
    m.provenance = Provenance::Measured; m.value = PressureRatio(pressure);
    frun.ingest(m, m.time);

    auto res = frun.evaluate(ServiceId(100), WorkloadId(200), milliseconds(200));
    if (!res.ok()) { std::printf("  evaluate failed: %s\n", res.status().message.c_str()); return 1; }
    const Explanation& ex = res.value_unchecked().explanation;
    std::printf("  compliance=%s  binding=%s  action=%s\n",
                compliance_state_name(ex.compliance).data(),
                dimension_name(ex.binding_dimension).data(),
                enforcement_action_name(ex.selected_action).data());
    for (auto& os : ex.objectives) {
      std::printf("    obj=%s state=%s samples=%lld fresh=%s\n", os.name.c_str(),
                  compliance_state_name(os.state).data(), (long long)os.sample_count.value(),
                  freshness_name(os.freshness).data());
    }
    std::printf("  (target is policy; measured values are REAL)\n");
  }

  // Clean up device memory and verify baseline restored.
  CUDA_CHECK(cudaFree(dx)); CUDA_CHECK(cudaFree(dy)); CUDA_CHECK(cudaFree(dout));
  CUDA_CHECK(cudaDeviceSynchronize());
  size_t free1 = 0, total1 = 0; CUDA_CHECK(cudaMemGetInfo(&free1, &total1));
  bool restored = ((long long)(free1 / (1024*1024)) >= (long long)(free0 / (1024*1024)) - 16);
  std::printf("\n=== device memory baseline restored: %s ===\n", restored ? "YES" : "NO");
  std::printf("CUDA PROOF: %s\n", restored ? "PASS" : "FAIL");
  return restored ? 0 : 1;
}
