#!/usr/bin/env bash
# 说明：最小化示例，运行analytical前端，加载带有重构事件的trace。

set -euo pipefail

# 假设构建完成，使用congestion_aware前端二进制
BIN="./astra-sim/network_frontend/analytical/congestion_aware/astra_analytical"

# 输入配置路径（示例）
WORKLOAD_CFG="examples/workload/workload.json"
COMM_GROUP_CFG="examples/workload/comm_group_empty.json"
SYSTEM_CFG="examples/system/system.json"
REMOTE_MEM_CFG="examples/remote_mem/remote_mem.json"
NETWORK_CFG="extern/network_backend/analytical/input/Ring.yml"
LOGGING_CFG="examples/logging/logging.json"

# 复制测试trace（这里仅做示例，不实际运行ET生成）
TRACE_JSON="extern/graph_frontend/chakra/tests/reconfig_trace.json"
echo "Using trace: ${TRACE_JSON}"

${BIN} \
  --workload-configuration ${TRACE_JSON} \
  --comm-group-configuration ${COMM_GROUP_CFG} \
  --system-configuration ${SYSTEM_CFG} \
  --remote-memory-configuration ${REMOTE_MEM_CFG} \
  --network-configuration ${NETWORK_CFG} \
  --logging-configuration ${LOGGING_CFG} \
  --num-queues-per-dim 1 \
  --comm-scale 1.0 \
  --injection-scale 1.0 \
  --rendezvous-protocol false

echo "Done."
