/*
MIT License

Copyright(c) 2020 Futurewei Cloud

    Permission is hereby granted,
    free of charge, to any person obtaining a copy of this software and associated documentation files(the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and / or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions :

    The above copyright notice and this permission notice shall be included in all copies
    or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS",
    WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER
    LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.
*/

#include "CPOService.h"

#include <k2/appbase/Appbase.h>
#include <k2/infrastructure/APIServer.h>
#include <k2/transport/Payload.h>  // for payload construction
#include <k2/transport/Status.h>  // for RPC
#include <k2/transport/RPCDispatcher.h>  // for RPC
#include <k2/dto/ControlPlaneOracle.h> // our DTO
#include <k2/dto/AssignmentManager.h> // our DTO
#include <k2/dto/MessageVerbs.h> // our DTO
#include <k2/dto/K23SI.h> // our DTO
#include <k2/transport/PayloadFileUtil.h>

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace k2 {

CPOService::CPOService(DistGetter distGetter) : _dist(distGetter) {
    K2LOG_I(log::cposvr, "ctor");
}

CPOService::~CPOService() {
    K2LOG_I(log::cposvr, "dtor");
}

seastar::future<> CPOService::gracefulStop() {
    K2LOG_I(log::cposvr, "stop");
    std::vector<seastar::future<>> futs;
    for (auto& [k,v]: _assignments) {
        futs.push_back(std::move(v));
    }
    _assignments.clear();
    return seastar::when_all_succeed(futs.begin(), futs.end()).discard_result();
}

seastar::future<> CPOService::start() {
    APIServer& api_server = AppBase().getDist<APIServer>().local();

    K2LOG_I(log::cposvr, "Registering message handlers");
    RPC().registerRPCObserver<dto::CollectionCreateRequest, dto::CollectionCreateResponse>(dto::Verbs::CPO_COLLECTION_CREATE, [this](dto::CollectionCreateRequest&& request) {
        return _dist().invoke_on(0, &CPOService::handleCreate, std::move(request));
    });
    api_server.registerAPIObserver<dto::CollectionCreateRequest, dto::CollectionCreateResponse>("CollectionCreate", "CPO CollectionCreate", [this] (dto::CollectionCreateRequest&& request) {
        return _dist().invoke_on(0, &CPOService::handleCreate, std::move(request));
    });

    RPC().registerRPCObserver<dto::CollectionGetRequest, dto::CollectionGetResponse>(dto::Verbs::CPO_COLLECTION_GET, [this](dto::CollectionGetRequest&& request) {
        return _dist().invoke_on(0, &CPOService::handleGet, std::move(request));
    });
    api_server.registerAPIObserver<dto::CollectionGetRequest, dto::CollectionGetResponse>("CollectionGet", "CPO CollectionGet", [this](dto::CollectionGetRequest&& request) {
        return _dist().invoke_on(0, &CPOService::handleGet, std::move(request));
    });

    RPC().registerRPCObserver<dto::PersistenceClusterCreateRequest, dto::PersistenceClusterCreateResponse>(dto::Verbs::CPO_PERSISTENCE_CLUSTER_CREATE, [this](dto::PersistenceClusterCreateRequest&& request) {
        return _dist().invoke_on(0, &CPOService::handlePersistenceClusterCreate, std::move(request));
    });

    RPC().registerRPCObserver<dto::PersistenceClusterGetRequest, dto::PersistenceClusterGetResponse>(dto::Verbs::CPO_PERSISTENCE_CLUSTER_GET, [this](dto::PersistenceClusterGetRequest&& request) {
        return _dist().invoke_on(0, &CPOService::handlePersistenceClusterGet, std::move(request));
    });

    RPC().registerRPCObserver<dto::CreateSchemaRequest, dto::CreateSchemaResponse>(dto::Verbs::CPO_SCHEMA_CREATE, [this] (dto::CreateSchemaRequest&& request) {
        return _dist().invoke_on(0, &CPOService::handleCreateSchema, std::move(request));
    });
    api_server.registerAPIObserver<dto::CreateSchemaRequest, dto::CreateSchemaResponse>("SchemaCreate", "CPO SchemaCreate", [this] (dto::CreateSchemaRequest&& request) {
        return _dist().invoke_on(0, &CPOService::handleCreateSchema, std::move(request));
    });

    RPC().registerRPCObserver<dto::GetSchemasRequest, dto::GetSchemasResponse>(dto::Verbs::CPO_SCHEMAS_GET,
    [this] (dto::GetSchemasRequest&& request) {
        return _dist().invoke_on(0, &CPOService::handleSchemasGet, std::move(request));
    });
    api_server.registerAPIObserver<dto::GetSchemasRequest, dto::GetSchemasResponse>("GetSchemas", "CPO get all schemas for a collection",
    [this] (dto::GetSchemasRequest&& request) {
        return _dist().invoke_on(0, &CPOService::handleSchemasGet, std::move(request));
    });

    if (seastar::this_shard_id() == 0) {
        // only core 0 handles CPO business
        if (!fileutil::makeDir(_dataDir())) {
            throw std::runtime_error("unable to create data directory");
        }
    }

    return seastar::make_ready_future<>();
}

int makeRangePartitionMap(dto::Collection& collection, const std::vector<String>& eps, const std::vector<String>& rangeEnds) {
    String lastEnd = "";

    if (rangeEnds.size() != eps.size()) {
        K2LOG_W(log::cposvr, "Error in client's make collection request: collection endpoints size does not equal rangeEnds size");
        return -1;
    }
    // The partition for a key is found using lower_bound on the start keys,
    // so it is OK for the last range end key to be ""
    if (rangeEnds[rangeEnds.size()-1] != "") {
        K2LOG_W(log::cposvr, "Error in client's make collection request: the last rangeEnd key is not an empty string");
        return -1;
    }

    for (uint64_t i = 0; i < eps.size(); ++i) {
        dto::Partition part {
            .pvid{
                .id = i,
                .rangeVersion=1,
                .assignmentVersion=1
            },
            .startKey=lastEnd,
            .endKey=rangeEnds[i],
            .endpoints={eps[i]},
            .astate=dto::AssignmentState::PendingAssignment
        };

        lastEnd = rangeEnds[i];
        collection.partitionMap.partitions.push_back(std::move(part));
    }

    collection.partitionMap.version++;
    return 0;
}

void makeHashPartitionMap(dto::Collection& collection, const std::vector<String>& eps) {
    const uint64_t max = std::numeric_limits<uint64_t>::max();

    uint64_t partSize = (eps.size() > 0) ? (max / eps.size()) : (max);
    for (uint64_t i =0; i < eps.size(); ++i) {
        uint64_t start = i*partSize;
        uint64_t end = (i == eps.size() -1) ? (max) : ((i+1)*partSize - 1);

        dto::Partition part{
            .pvid{
                .id = i,
                .rangeVersion=1,
                .assignmentVersion=1
            },
            .startKey=std::to_string(start),
            .endKey=std::to_string(end),
            .endpoints={eps[i]},
            .astate=dto::AssignmentState::PendingAssignment
        };
        collection.partitionMap.partitions.push_back(std::move(part));
    }

    collection.partitionMap.version++;
}

seastar::future<std::tuple<Status, dto::CollectionCreateResponse>>
CPOService::handleCreate(dto::CollectionCreateRequest&& request) {
    K2LOG_I(log::cposvr, "Received collection create request for name={}", request.metadata.name);
    auto cpath = _getCollectionPath(request.metadata.name);
    if (fileutil::fileExists(cpath)) {
        return RPCResponse(Statuses::S403_Forbidden("collection already exists"), dto::CollectionCreateResponse());
    }
    request.metadata.heartbeatDeadline = _collectionHeartbeatDeadline();
    // create a collection from the incoming request
    dto::Collection collection;
    collection.metadata = request.metadata;

    int err = 0;
    if (collection.metadata.hashScheme == dto::HashScheme::HashCRC32C) {
        makeHashPartitionMap(collection, request.clusterEndpoints);
    }
    else if (collection.metadata.hashScheme == dto::HashScheme::Range) {
        err = makeRangePartitionMap(collection, request.clusterEndpoints, request.rangeEnds);
    }
    else {
        return RPCResponse(Statuses::S403_Forbidden("Unknown hashScheme"), dto::CollectionCreateResponse());
    }

    if (err) {
        return RPCResponse(Statuses::S400_Bad_Request("Bad rangeEnds"), dto::CollectionCreateResponse());
    }

    schemas[collection.metadata.name] = std::vector<dto::Schema>();

    auto status = _saveCollection(collection);
    if (!status.is2xxOK()) {
        return RPCResponse(std::move(status), dto::CollectionCreateResponse());
    }

    K2LOG_I(log::cposvr, "Created collection {}", cpath);
    _assignCollection(collection);
    return RPCResponse(std::move(status), dto::CollectionCreateResponse());
}

seastar::future<std::tuple<Status, dto::CollectionGetResponse>>
CPOService::handleGet(dto::CollectionGetRequest&& request) {
    K2LOG_I(log::cposvr, "Received collection get request for {}", request.name);
    auto [status, collection] = _getCollection(request.name);

    dto::CollectionGetResponse response;
    if (status.is2xxOK()) {
        response.collection = std::move(collection);
    }
    return RPCResponse(std::move(status), std::move(response));
}

seastar::future<Status> CPOService::_pushSchema(const dto::Collection& collection, const dto::Schema& schema) {
    std::vector<seastar::future<std::tuple<Status, dto::K23SIPushSchemaResponse>>> pushFutures;

    for (const dto::Partition& part : collection.partitionMap.partitions) {
        auto endpoint = RPC().getTXEndpoint(*(part.endpoints.begin()));
        if (!endpoint) {
            return seastar::make_ready_future<Status>(Statuses::S500_Internal_Server_Error("Partition endpoint was null"));
        }

        dto::K23SIPushSchemaRequest request { collection.metadata.name, schema };

        pushFutures.push_back(RPC().callRPC<dto::K23SIPushSchemaRequest, dto::K23SIPushSchemaResponse>
            (dto::Verbs::K23SI_PUSH_SCHEMA, request, *endpoint, 1s));
    }

    return when_all_succeed(pushFutures.begin(), pushFutures.end())
    .then([] (auto&& doneFutures) {
        for (const auto& doneFuture : doneFutures) {
            auto& [status, k2response] = doneFuture;
            if (!status.is2xxOK()) {
                K2LOG_W(log::cposvr, "Failed to push schema update: status={}", status);
                return seastar::make_ready_future<Status>(status);
            }
        }

        return seastar::make_ready_future<Status>(Statuses::S200_OK(""));
    })
    .handle_exception([](auto exc) {
        K2LOG_W_EXC(log::cposvr, exc, "Failed to push schema update");
        return seastar::make_ready_future<Status>(Statuses::S500_Internal_Server_Error("Failed to push schema"));
    });
}

seastar::future<std::tuple<Status, dto::GetSchemasResponse>>
CPOService::handleSchemasGet(dto::GetSchemasRequest&& request) {
    auto it = schemas.find(request.collectionName);
    if (it == schemas.end()) {
        auto [status, collection] = _getCollection(request.collectionName);
        if (!status.is2xxOK()) {
            return RPCResponse(std::move(status), dto::GetSchemasResponse{});
        }

        it = schemas.find(request.collectionName);
        K2ASSERT(log::cposvr, it != schemas.end(), "Schemas iterator is end after collection refresh");
    }

    return RPCResponse(Statuses::S200_OK("SchemasGet success"), dto::GetSchemasResponse { it->second });
}

seastar::future<std::tuple<Status, dto::CreateSchemaResponse>>
CPOService::handleCreateSchema(dto::CreateSchemaRequest&& request) {
    K2LOG_I(log::cposvr, "Received schema create request for {} : {}", request.collectionName, request.schema.name);

    // 1. Stateless validation of request

    Status validation = request.schema.basicValidation();
    if (!validation.is2xxOK()) {
        return RPCResponse(std::move(validation), dto::CreateSchemaResponse{});
    }

    // 2. Stateful validation

    // getCollection to make sure in memory cache of schemas is up to date, and we need the
    // collection with partition map anyway to do the schema push
    auto [status, collection] = _getCollection(request.collectionName);
    if (!status.is2xxOK()) {
        return RPCResponse(std::move(status), dto::CreateSchemaResponse{});
    }

    bool validatedKeys = false;
    for (const dto::Schema& otherSchema : schemas[request.collectionName]) {
        if (otherSchema.name == request.schema.name && otherSchema.version == request.schema.version) {
            return RPCResponse(Statuses::S403_Forbidden("Schema name and version already exist"), dto::CreateSchemaResponse{});
        }

        // As long as we validate that the key fields match with at least one
        // other schema with the same name then they will always match
        if (!validatedKeys && otherSchema.name == request.schema.name) {
            validation = otherSchema.canUpgradeTo(request.schema);
            if (!validation.is2xxOK()) {
                return RPCResponse(std::move(validation), dto::CreateSchemaResponse{});
            }

            validatedKeys = true;
        }
    }

    // 3. Save to disk
    Status saved = _saveSchemas(request.collectionName);
    if (!saved.is2xxOK()) {
        return RPCResponse(std::move(saved), dto::CreateSchemaResponse{});
    }

    // 4. Update in memory
    schemas[request.collectionName].push_back(std::move(request.schema));

    // 5. Push to K2 nodes and respond to client
    return _pushSchema(collection, schemas[request.collectionName].back())
    .then([] (Status&& status) {
        return RPCResponse(std::move(status), dto::CreateSchemaResponse{});
    });
}

String CPOService::_getCollectionPath(String name) {
    return _dataDir() + "/" + name + ".collection";
}

String CPOService::_getPersistenceClusterPath(String clusterName) {
    return _dataDir() + "/" + clusterName + ".persistence";
}

String CPOService::_getSchemasPath(String collectionName) {
    return _getCollectionPath(collectionName) + ".schemas";
}

void CPOService::_assignCollection(dto::Collection& collection) {
    auto &name = collection.metadata.name;
    K2LOG_I(log::cposvr, "Assigning collection {}, to {} nodes", name, collection.partitionMap.partitions.size());
    std::vector<seastar::future<>> futs;
    for (auto& part : collection.partitionMap.partitions) {
        if (part.endpoints.size() == 0) {
            K2LOG_E(log::cposvr, "empty endpoint for partition assignment: {}", part);
            continue;
        }
        auto ep = *part.endpoints.begin();
        K2LOG_I(log::cposvr, "Assigning collection {}, to {}", name, part);
        auto txep = RPC().getTXEndpoint(ep);
        if (!txep) {
            K2LOG_W(log::cposvr, "unable to obtain endpoint for {}", ep);
            continue;
        }
        dto::AssignmentCreateRequest request;
        request.collectionMeta = collection.metadata;
        request.partition = part;

        K2LOG_I(log::cposvr, "Sending assignment for partition: {}", request.partition);
        futs.push_back(
        RPC().callRPC<dto::AssignmentCreateRequest, dto::AssignmentCreateResponse>
                (dto::K2_ASSIGNMENT_CREATE, request, *txep, _assignTimeout())
        .then([this, name, ep](auto&& result) {
            auto& [status, resp] = result;
            if (status.is2xxOK()) {
                K2LOG_I(log::cposvr, "assignment successful for collection {}, for partition {}", name, resp.assignedPartition);
                _handleCompletedAssignment(name, std::move(resp));
            }
            else {
                // The node refused to accept the assignment. For now, just ignore this
                K2LOG_W(log::cposvr, "assignment for collection {} was refused by {}, due to: {}", name, ep, status);
                _handleCompletedAssignment(name, std::move(resp));
            }
            return seastar::make_ready_future();
        })
        );
    }
    _assignments.emplace(name, seastar::when_all_succeed(futs.begin(), futs.end()).discard_result()
        .then([this, name] {
            _assignments.erase(name);
            return seastar::make_ready_future();
        }));
}

void CPOService::_handleCompletedAssignment(const String& cname, dto::AssignmentCreateResponse&& request) {
    auto [status, haveCollection] = _getCollection(cname);
    if (!status.is2xxOK()) {
        K2LOG_E(log::cposvr, "unable to find collection which reported assignment {}", cname);
        return;
    }
    for (auto& part: haveCollection.partitionMap.partitions) {
        if (part.startKey == request.assignedPartition.startKey &&
            part.endKey == request.assignedPartition.endKey &&
            part.pvid.id == request.assignedPartition.pvid.id &&
            part.pvid.assignmentVersion == request.assignedPartition.pvid.assignmentVersion &&
            part.pvid.rangeVersion == request.assignedPartition.pvid.rangeVersion) {
                K2LOG_I(log::cposvr, "Assignment received for active partition {}", request.assignedPartition);
                part.astate = request.assignedPartition.astate;
                part.endpoints = std::move(request.assignedPartition.endpoints);
                _saveCollection(haveCollection);
                return;
        }
    }
    K2LOG_E(log::cposvr, "assignment completion does not match any stored partitions: {}", request.assignedPartition);
}

std::tuple<Status, dto::Collection> CPOService::_getCollection(String name) {
    auto cpath = _getCollectionPath(name);
    std::tuple<Status, dto::Collection> result;
    Payload p;
    if (!fileutil::readFile(p, cpath)) {
        std::get<0>(result) = Statuses::S404_Not_Found("collection not found");
        return result;
    }
    if (!p.read(std::get<1>(result))) {
        std::get<0>(result) = Statuses::S500_Internal_Server_Error("unable to read collection data");
        return result;
    };
    K2LOG_I(log::cposvr, "Found collection in: {}", cpath);
    std::get<0>(result) = Statuses::S200_OK("collection found");

    // Check to see if we need to load schemas from file too
    auto it = schemas.find(name);
    if (it == schemas.end()) {
        Status schemaStatus = _loadSchemas(name);
        if (!schemaStatus.is2xxOK()) {
            std::get<0>(result) = Statuses::S500_Internal_Server_Error("unable to read schema data");
            return result;
        }
    }

    return result;
}

Status CPOService::_loadSchemas(const String& collectionName) {
    auto cpath = _getSchemasPath(collectionName);
    Payload p;
    std::vector<dto::Schema> loadedSchemas;
    if (!fileutil::readFile(p, cpath)) {
        return Statuses::S404_Not_Found("schemas not found");
    }
    if (!p.read(loadedSchemas)) {
        return Statuses::S500_Internal_Server_Error("unable to read schema data");
    }

    schemas[collectionName] = std::move(loadedSchemas);

    return Statuses::S200_OK("Schemas loaded");
}

Status CPOService::_saveCollection(dto::Collection& collection) {
    auto cpath = _getCollectionPath(collection.metadata.name);
    Payload p([] { return Binary(4096); });
    p.write(collection);
    if (!fileutil::writeFile(std::move(p), cpath)) {
        return Statuses::S500_Internal_Server_Error("unable to write collection data");
    }

    K2LOG_D(log::cposvr, "saved collection: {}", cpath);
    return _saveSchemas(collection.metadata.name);
}

Status CPOService::_saveSchemas(const String& collectionName) {
    auto cpath = _getSchemasPath(collectionName);
    Payload p([] { return Binary(4096); });
    p.write(schemas[collectionName]);
    if (!fileutil::writeFile(std::move(p), cpath)) {
        return Statuses::S500_Internal_Server_Error("unable to write schema data");
    }

    K2LOG_D(log::cposvr, "saved schemas: {}", cpath);
    return Statuses::S201_Created("schema written");
}

seastar::future<std::tuple<Status, dto::PersistenceClusterCreateResponse>>
CPOService::handlePersistenceClusterCreate(dto::PersistenceClusterCreateRequest&& request){
    K2LOG_I(log::cposvr, "Received persistence cluster create request for {}", request.cluster.name);
    auto cpath = _getPersistenceClusterPath(request.cluster.name);
    dto::PersistenceCluster persistenceCluster;
    Payload p;
    if (fileutil::readFile(p, cpath)) {
        return RPCResponse(Statuses::S409_Conflict("persistence cluster already exists"), dto::PersistenceClusterCreateResponse());
    }

    Payload q([] { return Binary(4096); });
    q.write(request.cluster);
    if (!fileutil::writeFile(std::move(q), cpath)) {
        return RPCResponse(Statuses::S500_Internal_Server_Error("unable to write persistence cluster data"), dto::PersistenceClusterCreateResponse());
    }
    return RPCResponse(Statuses::S201_Created("persistence cluster creates successfully"), dto::PersistenceClusterCreateResponse());
}

seastar::future<std::tuple<Status, dto::PersistenceClusterGetResponse>>
CPOService::handlePersistenceClusterGet(dto::PersistenceClusterGetRequest&& request) {
    K2LOG_I(log::cposvr, "Received persistence cluster get request with name {}", request.name);
    auto cpath = _getPersistenceClusterPath(request.name);
    dto::PersistenceCluster persistenceCluster;
    Payload p;
    if (!fileutil::readFile(p, cpath)) {
        return RPCResponse(Statuses::S404_Not_Found("persistence cluster not found"), dto::PersistenceClusterGetResponse());
    }
    if (!p.read(persistenceCluster)) {
        return RPCResponse(Statuses::S500_Internal_Server_Error("unable to read persistence cluster data"), dto::PersistenceClusterGetResponse());
    };

    K2LOG_I(log::cposvr, "Found persistence cluster in: {}", cpath);
    dto::PersistenceClusterGetResponse response{.cluster=std::move(persistenceCluster)};
    return RPCResponse(Statuses::S200_OK("persistence cluster found"), std::move(response));
}

} // namespace k2
