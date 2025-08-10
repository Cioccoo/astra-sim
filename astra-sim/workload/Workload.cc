/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#include "astra-sim/workload/Workload.hh"

#include "astra-sim/common/Logging.hh"
#include "astra-sim/system/IntData.hh"
#include "astra-sim/system/MemEventHandlerData.hh"
#include "astra-sim/system/RecvPacketEventHandlerData.hh"
#include "astra-sim/system/SendPacketEventHandlerData.hh"
#include "astra-sim/system/WorkloadLayerHandlerData.hh"
#include <json/json.hpp>

#include <iostream>
#include <stdlib.h>
#include <unistd.h>

using namespace std;
using namespace AstraSim;
using namespace Chakra;
using json = nlohmann::json;

typedef ChakraProtoMsg::NodeType ChakraNodeType;
typedef ChakraProtoMsg::CollectiveCommType ChakraCollectiveCommType;

// 新增：用于保存一次拓扑重构的参数
struct ReconfigurationParams {
    // 目标拓扑类型名称（例如："Ring"、"FullyConnected"、"Switch"）
    std::string target_topology_name;
    // 重构延迟（单位：ns），用于通过事件系统建模重构耗时
    uint64_t reconfiguration_delay_ns = 0;
    // 是否在重构期间暂停通信（新通信不再发起）
    bool pause_communication = false;
    // 如果需要暂停，暂停持续时长（单位：ns），如需与重构延迟分离
    uint64_t pause_duration_ns = 0;
};

// 前向声明：解析和处理重构
static ReconfigurationParams parse_reconfiguration_params(
    const std::shared_ptr<Chakra::ETFeederNode>& node);
static void normalize_and_validate_reconfig_params(ReconfigurationParams& params);

Workload::Workload(Sys* sys, string et_filename, string comm_group_filename) {
    string workload_filename = et_filename + "." + to_string(sys->id) + ".et";
    // Check if workload filename exists
    if (access(workload_filename.c_str(), R_OK) < 0) {
        string error_msg;
        if (errno == ENOENT) {
            error_msg =
                "workload file: " + workload_filename + " does not exist";
        } else if (errno == EACCES) {
            error_msg = "workload file: " + workload_filename +
                        " exists but is not readable";
        } else {
            error_msg =
                "Unknown workload file: " + workload_filename + " access error";
        }
        LoggerFactory::get_logger("workload")->critical(error_msg);
        exit(EXIT_FAILURE);
    }
    this->et_feeder = new ETFeeder(workload_filename);
    this->comm_group = nullptr;
    // TODO: parametrize the number of available hardware resources
    this->hw_resource = new HardwareResource(1);
    this->sys = sys;
    initialize_comm_group(comm_group_filename);
    this->is_finished = false;
}

Workload::~Workload() {
    if (this->comm_group != nullptr) {
        delete this->comm_group;
    }
    if (this->et_feeder != nullptr) {
        delete this->et_feeder;
    }
    if (this->hw_resource != nullptr) {
        delete this->hw_resource;
    }
}

void Workload::initialize_comm_group(string comm_group_filename) {
    // communicator group input file is not given
    if (comm_group_filename.find("empty") != std::string::npos) {
        comm_group = nullptr;
        return;
    }

    ifstream inFile;
    json j;
    inFile.open(comm_group_filename);
    inFile >> j;

    for (json::iterator it = j.begin(); it != j.end(); ++it) {
        bool in_comm_group = false;

        for (auto id : it.value()) {
            if (id == sys->id) {
                in_comm_group = true;
            }
        }

        if (in_comm_group) {
            std::vector<int> involved_NPUs;
            for (auto id : it.value()) {
                involved_NPUs.push_back(id);
            }
            comm_group = new CommunicatorGroup(1, involved_NPUs, sys);
            // Note: All NPUs should create comm group with identical ids if
            // they want to communicate with each other
        }
    }
}

void Workload::issue_dep_free_nodes() {
    std::queue<shared_ptr<Chakra::ETFeederNode>> push_back_queue;
    shared_ptr<Chakra::ETFeederNode> node = et_feeder->getNextIssuableNode();
    while (node != nullptr) {
        if (hw_resource->is_available(node)) {
            issue(node);
        } else {
            push_back_queue.push(node);
        }
        node = et_feeder->getNextIssuableNode();
    }

    while (!push_back_queue.empty()) {
        shared_ptr<Chakra::ETFeederNode> node = push_back_queue.front();
        et_feeder->pushBackIssuableNode(node->id());
        push_back_queue.pop();
    }
}

void Workload::issue(shared_ptr<Chakra::ETFeederNode> node) {
    auto logger = LoggerFactory::get_logger("workload");
    if (sys->replay_only) {
        hw_resource->occupy(node);
        issue_replay(node);
    } else {
        if ((node->type() == ChakraNodeType::MEM_LOAD_NODE) ||
            (node->type() == ChakraNodeType::MEM_STORE_NODE)) {
            if (sys->trace_enabled) {
                logger->debug("issue,sys->id={}, tick={}, node->id={}, "
                              "node->name={}, node->type={}",
                              sys->id, Sys::boostedTick(), node->id(),
                              node->name(),
                              static_cast<uint64_t>(node->type()));
            }
            issue_remote_mem(node);
        } else if (node->is_cpu_op() ||
                   (!node->is_cpu_op() &&
                    node->type() == ChakraNodeType::COMP_NODE)) {
            if ((node->runtime() == 0) && (node->num_ops() == 0)) {
                skip_invalid(node);
            } else {
                if (sys->trace_enabled) {
                    logger->debug("issue,sys->id={}, tick={}, node->id={}, "
                                  "node->name={}, node->type={}",
                                  sys->id, Sys::boostedTick(), node->id(),
                                  node->name(),
                                  static_cast<uint64_t>(node->type()));
                }
                issue_comp(node);
            }
        } else if (!node->is_cpu_op() &&
                   (node->type() == ChakraNodeType::COMM_COLL_NODE ||
                    (node->type() == ChakraNodeType::COMM_SEND_NODE) ||
                    (node->type() == ChakraNodeType::COMM_RECV_NODE))) {
            if (sys->trace_enabled) {
                if (sys->trace_enabled) {
                    logger->debug("issue,sys->id={}, tick={}, node->id={}, "
                                  "node->name={}, node->type={}",
                                  sys->id, Sys::boostedTick(), node->id(),
                                  node->name(),
                                  static_cast<uint64_t>(node->type()));
                }
            }
            issue_comm(node);
        } else if (node->type() == ChakraNodeType::INVALID_NODE) {
            skip_invalid(node);
        }
    }
}

void Workload::issue_replay(shared_ptr<Chakra::ETFeederNode> node) {
    WorkloadLayerHandlerData* wlhd = new WorkloadLayerHandlerData;
    wlhd->node_id = node->id();
    uint64_t runtime = 1ul;
    if (node->runtime() != 0ul) {
        // chakra runtimes are in microseconds and we should convert it into
        // nanoseconds
        runtime = node->runtime() * 1000;
    }
    if (node->is_cpu_op()) {
        hw_resource->tics_cpu_ops += runtime;
    } else {
        hw_resource->tics_gpu_ops += runtime;
    }
    sys->register_event(this, EventType::General, wlhd, runtime);
}

void Workload::issue_remote_mem(shared_ptr<Chakra::ETFeederNode> node) {
    hw_resource->occupy(node);

    WorkloadLayerHandlerData* wlhd = new WorkloadLayerHandlerData;
    wlhd->sys_id = sys->id;
    wlhd->workload = this;
    wlhd->node_id = node->id();
    sys->remote_mem->issue(node->tensor_size(), wlhd);
}

void Workload::issue_comp(shared_ptr<Chakra::ETFeederNode> node) {
    hw_resource->occupy(node);

    if (sys->roofline_enabled) {
        WorkloadLayerHandlerData* wlhd = new WorkloadLayerHandlerData;
        wlhd->node_id = node->id();

        double operational_intensity = static_cast<double>(node->num_ops()) /
                                       static_cast<double>(node->tensor_size());
        double perf = sys->roofline->get_perf(operational_intensity);
        double elapsed_time =
            static_cast<double>(node->num_ops()) / perf;  // sec
        uint64_t runtime =
            static_cast<uint64_t>(elapsed_time * 1e9);  // sec -> ns
        if (node->is_cpu_op()) {
            hw_resource->tics_cpu_ops += runtime;
        } else {
            hw_resource->tics_gpu_ops += runtime;
        }
        sys->register_event(this, EventType::General, wlhd, runtime);
    } else {
        // advance this node forward the recorded "replayed" time specificed in
        // the ET.
        issue_replay(node);
    }
}

void Workload::issue_comm(shared_ptr<Chakra::ETFeederNode> node) {
    hw_resource->occupy(node);

    vector<bool> involved_dim;

    if (node->has_other_attr("involved_dim")) {
        const ChakraProtoMsg::AttributeProto& attr =
            node->get_other_attr("involved_dim");

        // Ensure the attribute is of type bool_list before accessing
        if (attr.has_bool_list()) {
            const ChakraProtoMsg::BoolList& bool_list = attr.bool_list();

            // Traverse bool_list and add values to involved_dim
            for (int i = 0; i < bool_list.values_size(); ++i) {
                involved_dim.push_back(bool_list.values(i));
            }
        } else {
            cerr << "Expected bool_list in involved_dim but found another type."
                 << endl;
            exit(EXIT_FAILURE);
        }
    } else {
        // involved_dim does not exist in ETFeeder.
        // Assume involved_dim = [1,1,1,1,1] which we could simulate 5-Dimension.
	// Could use Process Group to build involved_dim later. 
	// Once process group is implemented, you should get
        // that with node->pg_name()
	
	for(int i = 0; i < 4; i++)
            involved_dim.push_back(true);
    }

    if (!node->is_cpu_op() &&
        (node->type() == ChakraNodeType::COMM_COLL_NODE)) {
        // 兼容旧proto：以整数值比较（10表示RECONFIGURATION）
        if (static_cast<int>(node->comm_type()) == 10) {
            // 新增：处理拓扑重构
            issue_reconfiguration(node);
        } else if (node->comm_type() == ChakraCollectiveCommType::ALL_REDUCE) {
            DataSet* fp =
                sys->generate_all_reduce(node->comm_size(), involved_dim,
                                         comm_group, node->comm_priority());
            collective_comm_node_id_map[fp->my_id] = node->id();
            collective_comm_wrapper_map[fp->my_id] = fp;
            fp->set_notifier(this, EventType::CollectiveCommunicationFinished);

        } else if (node->comm_type() == ChakraCollectiveCommType::ALL_TO_ALL) {
            DataSet* fp =
                sys->generate_all_to_all(node->comm_size(), involved_dim,
                                         comm_group, node->comm_priority());
            collective_comm_node_id_map[fp->my_id] = node->id();
            collective_comm_wrapper_map[fp->my_id] = fp;
            fp->set_notifier(this, EventType::CollectiveCommunicationFinished);

        } else if (node->comm_type() == ChakraCollectiveCommType::ALL_GATHER) {
            DataSet* fp =
                sys->generate_all_gather(node->comm_size(), involved_dim,
                                         comm_group, node->comm_priority());
            collective_comm_node_id_map[fp->my_id] = node->id();
            collective_comm_wrapper_map[fp->my_id] = fp;
            fp->set_notifier(this, EventType::CollectiveCommunicationFinished);

        } else if (node->comm_type() ==
                   ChakraCollectiveCommType::REDUCE_SCATTER) {
            DataSet* fp =
                sys->generate_reduce_scatter(node->comm_size(), involved_dim,
                                             comm_group, node->comm_priority());
            collective_comm_node_id_map[fp->my_id] = node->id();
            collective_comm_wrapper_map[fp->my_id] = fp;
            fp->set_notifier(this, EventType::CollectiveCommunicationFinished);

        } else if (node->comm_type() == ChakraCollectiveCommType::BROADCAST) {
            // broadcast colelctive has not been implemented in ASTRA-SIM yet.
            // So, we just use its real system mesurements
            uint64_t runtime = 1ul;
            if (node->runtime() != 0ul) {
                // chakra runtimes are in microseconds and we should convert it
                // into nanoseconds
                runtime = node->runtime() * 1000;
            }
            DataSet* fp = new DataSet(1);
            fp->set_notifier(this, EventType::CollectiveCommunicationFinished);
            collective_comm_node_id_map[fp->my_id] = node->id();
            collective_comm_wrapper_map[fp->my_id] = fp;
            sys->register_event(fp, EventType::General, nullptr,
                                // chakra runtimes are in microseconds and we
                                // should convert it into nanoseconds
                                runtime);
            fp->set_notifier(this, EventType::CollectiveCommunicationFinished);
        }
    } else if (node->type() == ChakraNodeType::COMM_SEND_NODE) {
        sim_request snd_req;
        snd_req.srcRank = node->comm_src();
        snd_req.dstRank = node->comm_dst();
        snd_req.reqType = UINT8;
        SendPacketEventHandlerData* sehd = new SendPacketEventHandlerData;
        sehd->callable = this;
        sehd->wlhd = new WorkloadLayerHandlerData;
        sehd->wlhd->node_id = node->id();
        sehd->event = EventType::PacketSent;
        sys->front_end_sim_send(0, Sys::dummy_data, node->comm_size(), UINT8,
                                node->comm_dst(), node->comm_tag(), &snd_req,
                                Sys::FrontEndSendRecvType::NATIVE,
                                &Sys::handleEvent, sehd);
    } else if (node->type() == ChakraNodeType::COMM_RECV_NODE) {
        sim_request rcv_req;
        RecvPacketEventHandlerData* rcehd = new RecvPacketEventHandlerData;
        rcehd->wlhd = new WorkloadLayerHandlerData;
        rcehd->wlhd->node_id = node->id();
        rcehd->workload = this;
        rcehd->event = EventType::PacketReceived;
        sys->front_end_sim_recv(0, Sys::dummy_data, node->comm_size(), UINT8,
                                node->comm_src(), node->comm_tag(), &rcv_req,
                                Sys::FrontEndSendRecvType::NATIVE,
                                &Sys::handleEvent, rcehd);
    } else {
        LoggerFactory::get_logger("workload")
            ->critical("Unknown communication node type");
        exit(EXIT_FAILURE);
    }
}

void Workload::skip_invalid(shared_ptr<Chakra::ETFeederNode> node) {
    et_feeder->freeChildrenNodes(node->id());
    et_feeder->removeNode(node->id());
}

void Workload::call(EventType event, CallData* data) {
    if (is_finished) {
        return;
    }

    if (event == EventType::CollectiveCommunicationFinished) {
        IntData* int_data = (IntData*)data;
        uint64_t coll_comm_id = int_data->data;

        hw_resource->tics_gpu_comms += int_data->execution_time;
        uint64_t node_id = collective_comm_node_id_map[coll_comm_id];
        shared_ptr<Chakra::ETFeederNode> node = et_feeder->lookupNode(node_id);

        if (sys->trace_enabled) {
            LoggerFactory::get_logger("workload")
                ->debug("callback,sys->id={}, tick={}, node->id={}, "
                        "node->name={}, node->type={}",
                        sys->id, Sys::boostedTick(), node->id(), node->name(),
                        static_cast<uint64_t>(node->type()));
        }

        hw_resource->release(node);

        et_feeder->freeChildrenNodes(node_id);

        issue_dep_free_nodes();
      
        // The Dataset class provides statistics that should be used later to dump
        // more statistics in the workload layer
        delete collective_comm_wrapper_map[coll_comm_id];
        collective_comm_wrapper_map.erase(coll_comm_id);
        et_feeder->removeNode(node_id);

    } else {
        if (data == nullptr) {
            issue_dep_free_nodes();
        } else {
            WorkloadLayerHandlerData* wlhd = (WorkloadLayerHandlerData*)data;
            shared_ptr<Chakra::ETFeederNode> node =
                et_feeder->lookupNode(wlhd->node_id);

            if (sys->trace_enabled) {
                LoggerFactory::get_logger("workload")
                    ->debug("callback,sys->id={}, tick={}, node->id={}, "
                            "node->name={}, node->type={}",
                            sys->id, Sys::boostedTick(), node->id(),
                            node->name(), static_cast<uint64_t>(node->type()));
            }

            hw_resource->release(node);

            et_feeder->freeChildrenNodes(node->id());

            issue_dep_free_nodes();

            et_feeder->removeNode(wlhd->node_id);
            delete wlhd;
        }
    }

    if (!et_feeder->hasNodesToIssue() &&
        (hw_resource->num_in_flight_cpu_ops == 0) &&
        (hw_resource->num_in_flight_gpu_comp_ops == 0) &&
        (hw_resource->num_in_flight_gpu_comm_ops == 0)) {
        report();
        sys->comm_NI->sim_notify_finished();
        is_finished = true;
    }
}

void Workload::fire() {
    call(EventType::General, NULL);
}

void Workload::report() {
    Tick curr_tick = Sys::boostedTick();
    LoggerFactory::get_logger("workload")
        ->info("sys[{}] finished, {} cycles, exposed communication {} cycles.",
               sys->id, curr_tick, curr_tick - hw_resource->tics_gpu_ops);
}

// 新增：解析重构参数（从Chakra节点的attributes中读取）
static ReconfigurationParams parse_reconfiguration_params(
    const std::shared_ptr<Chakra::ETFeederNode>& node) {
    ReconfigurationParams params;
    // 读取目标拓扑名（必需）
    if (node->has_other_attr("target_topology")) {
        const auto& attr = node->get_other_attr("target_topology");
        if (attr.has_string_val()) {
            params.target_topology_name = attr.string_val();
        }
    }
    // 读取重构延迟（可选，单位ns）
    if (node->has_other_attr("reconfiguration_delay_ns")) {
        const auto& attr = node->get_other_attr("reconfiguration_delay_ns");
        if (attr.has_uint64_val()) {
            params.reconfiguration_delay_ns = attr.uint64_val();
        }
    }
    // 读取是否暂停通信（可选）
    if (node->has_other_attr("pause_communication")) {
        const auto& attr = node->get_other_attr("pause_communication");
        if (attr.has_bool_val()) {
            params.pause_communication = attr.bool_val();
        }
    }
    // 读取暂停持续时间（可选，单位ns）
    if (node->has_other_attr("pause_duration_ns")) {
        const auto& attr = node->get_other_attr("pause_duration_ns");
        if (attr.has_uint64_val()) {
            params.pause_duration_ns = attr.uint64_val();
        }
    }
    return params;
}

// 新增：校验并规范化重构参数
static void normalize_and_validate_reconfig_params(ReconfigurationParams& params) {
    // 默认拓扑名必须存在且合法
    if (params.target_topology_name != "Ring" &&
        params.target_topology_name != "FullyConnected" &&
        params.target_topology_name != "Switch") {
        std::cerr << "Invalid target_topology: " << params.target_topology_name << std::endl;
        std::exit(EXIT_FAILURE);
    }
    // 非负校验
    // 若用户未提供延迟，默认0ns
    // 若用户未提供暂停时长，默认等于重构延迟（更贴近实际）
    if (params.pause_communication && params.pause_duration_ns == 0) {
        params.pause_duration_ns = params.reconfiguration_delay_ns;
    }
}

// 新增：处理拓扑重构事件
void Workload::issue_reconfiguration(std::shared_ptr<Chakra::ETFeederNode> node) {
    // 占用硬件资源，保持与其他issue_*接口一致
    hw_resource->occupy(node);

    // 解析参数并规范化
    ReconfigurationParams params = parse_reconfiguration_params(node);
    normalize_and_validate_reconfig_params(params);

    // 若需要，先暂停通信（仅阻止新通信发起，已在途的允许自然完成）
    if (params.pause_communication) {
        sys->pause_all_communications();
    }

    // 将重构请求下发到系统层，由系统层与网络前端配合完成拓扑替换
    sys->request_network_reconfiguration(params.target_topology_name,
                                         params.reconfiguration_delay_ns,
                                         params.pause_communication,
                                         params.pause_duration_ns,
                                         node->id());
}
