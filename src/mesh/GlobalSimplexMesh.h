#ifndef GLOBALSIMPLEXMESH_H
#define GLOBALSIMPLEXMESH_H

#include "LocalFaces.h"
#include "LocalSimplexMesh.h"
#include "MeshData.h"
#include "Simplex.h"

#include "parallel/CommPattern.h"
#include "parallel/DistributedCSR.h"
#include "parallel/MPITraits.h"
#include "parallel/MetisPartitioner.h"
#include "parallel/SortedDistribution.h"
#include "util/Utility.h"

#include <mpi.h>

#include <array>
#include <cstddef>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tndm {

/**
 * @brief Class that holds a distributed D-simplex mesh.
 *
 * Global means that vertices and elements (an element is a D-simplex) exist only once in the
 * distributed memory space. Moreover, vertices and elements are distributed independently, that is,
 * the vertices required by an element may not reside on the same rank.
 *
 * One may attach vertex data and element data to a global mesh. If you need facet data or edge
 * data, then you have to add a boundary mesh. (The element data on the boundary mesh is then going
 * to be the edge or facet data.)
 *
 * @tparam D simplex dimension
 */
template <std::size_t D> class GlobalSimplexMesh {
public:
    using simplex_t = Simplex<D>;
    static_assert(sizeof(simplex_t) == (D + 1) * sizeof(uint64_t));
    template <std::size_t DD> using global_mesh_ptr = std::unique_ptr<GlobalSimplexMesh<DD>>;

    GlobalSimplexMesh(std::vector<simplex_t>&& elements,
                      std::unique_ptr<MeshData> vertexDat = nullptr,
                      std::unique_ptr<MeshData> elementDat = nullptr,
                      MPI_Comm comm = MPI_COMM_WORLD)
        : elems_(std::move(elements)), vertexData(std::move(vertexDat)),
          elementData(std::move(elementDat)), comm(comm), isPartitionedByHash(false) {
        if (vertexData) {
            vtxdist = makeSortedDistribution(vertexData->size());
        }
    }

    auto const& getElements() const { return elems_; }
    std::size_t numElements() const { return elems_.size(); }

    template <std::size_t DD> void setBoundaryMesh(global_mesh_ptr<DD> boundaryMesh) {
        static_assert(0 < DD && DD < D);
        std::get<DD>(boundaryMeshes) = std::move(boundaryMesh);
    }

    /**
     * @brief Mesh topology for partitioning.
     *
     * @tparam OutIntT Integer type of distributed CSR.
     *
     * @return Returns mesh in distributed CSR format as required by ParMETIS.
     */
    template <typename OutIntT> DistributedCSR<OutIntT> distributedCSR() const {
        DistributedCSR<OutIntT> csr;

        auto elmdist = makeSortedDistribution(numElements());
        csr.dist.resize(elmdist.size());
        std::copy(elmdist.begin(), elmdist.end(), csr.dist.begin());

        auto numElems = numElements();
        csr.rowPtr.resize(numElems + 1);
        csr.colInd.resize(numElems * (D + 1));

        OutIntT ind = 0;
        OutIntT ptr = 0;
        for (auto& e : elems_) {
            csr.rowPtr[ptr++] = ind;
            for (auto& p : e) {
                csr.colInd[ind++] = p;
            }
        }
        csr.rowPtr.back() = ind;

        return csr;
    }

    /**
     * @brief Use ParMETIS to optimise mesh partitioning.
     */
    void repartition() {
        auto distCSR = distributedCSR<idx_t>();
        auto partition = MetisPartitioner::partition(distCSR, D);

        doPartition(partition);
        isPartitionedByHash = false;
    }

    /**
     * @brief Partition elements by their hash value (SimplexHash).
     *
     * Should only be used for efficient element data queries. Otherwise use repartition().
     */
    void repartitionByHash() {
        if (isPartitionedByHash) {
            return;
        }
        std::vector<idx_t> partition;
        partition.reserve(numElements());
        auto plex2rank = getPlex2Rank<D>();
        for (auto& e : elems_) {
            partition.emplace_back(plex2rank(e));
        }

        doPartition(partition);
        isPartitionedByHash = true;
    }

    /**
     * @brief Local mesh construction with ghost entities.
     *
     * @param overlap Number of elements the partitions shall overlap
     *
     * @return
     */
    std::unique_ptr<LocalSimplexMesh<D>> getLocalMesh(unsigned overlap = 0) const {
        auto localFaces = getAllLocalFaces(overlap, std::make_index_sequence<D>{});

        return std::make_unique<LocalSimplexMesh<D>>(std::move(localFaces));
    }

private:
    template <std::size_t DD> friend class GlobalSimplexMesh;

    auto makeG2LMap() const {
        std::unordered_map<Simplex<D>, std::size_t, SimplexHash<D>> map;
        std::size_t local = 0;
        for (auto& e : elems_) {
            map[e] = local++;
        }
        return map;
    }

    void doPartition(std::vector<idx_t> const& partition) {
        int procs, rank;
        MPI_Comm_rank(comm, &rank);
        MPI_Comm_size(comm, &procs);
        assert(partition.size() == numElements());

        std::vector<std::size_t> enumeration(partition.size());
        std::iota(enumeration.begin(), enumeration.end(), std::size_t(0));

        std::sort(
            enumeration.begin(), enumeration.end(),
            [&partition](std::size_t a, std::size_t b) { return partition[a] < partition[b]; });

        std::vector<int> sendcounts(procs, 0);
        std::vector<simplex_t> elemsToSend;
        elemsToSend.reserve(numElements());
        auto eIt = enumeration.begin();
        for (int p = 0; p < procs; ++p) {
            while (eIt != enumeration.end() && partition[*eIt] <= p) {
                ++sendcounts[p];
                elemsToSend.emplace_back(elems_[*eIt]);
                ++eIt;
            }
        }
        assert(eIt == enumeration.end());

        AllToAllV a2a(std::move(sendcounts), comm);
        mpi_array_type<uint64_t> mpi_simplex_t(D + 1);
        elems_ = a2a.exchange(elemsToSend, mpi_simplex_t.get());

        if (elementData) {
            elementData = elementData->redistributed(enumeration, a2a);
        }
    }

    template <std::size_t... Is>
    auto getAllLocalFaces(unsigned overlap, std::index_sequence<Is...>) const {
        auto elemsCopy = getGhostElements(elems_, overlap);
        // Element contiguous GIDs only need prefix sum
        std::size_t ownedSize = numElements();
        std::size_t gidOffset;
        MPI_Scan(&ownedSize, &gidOffset, 1, mpi_type_t<std::size_t>(), MPI_SUM, comm);
        gidOffset -= ownedSize;
        std::vector<std::size_t> cGIDs(numElements());
        std::iota(cGIDs.begin(), cGIDs.end(), gidOffset);

        auto localFaces = std::make_tuple(getFaces<Is>(elemsCopy)...);
        return std::tuple_cat(std::move(localFaces), std::make_tuple(LocalFaces<D>(
                                                         std::move(elemsCopy), std::move(cGIDs))));
    }

    std::vector<Simplex<D>> getGhostElements(std::vector<Simplex<D>> elems,
                                             unsigned overlap) const {
        int procs;
        MPI_Comm_size(comm, &procs);

        bool hasDomainBoundaryFaces = false;
        std::unordered_set<Simplex<D - 1u>, SimplexHash<D - 1u>> domainBoundaryFaces;

        for (unsigned ol = 1; ol <= overlap; ++ol) {
            auto up = getBoundaryFaces(elems);
            if (hasDomainBoundaryFaces) {
                for (auto& face : domainBoundaryFaces) {
                    up.erase(face);
                }
            } else {
                domainBoundaryFaces = deleteDomainBoundaryFaces(up);
            }
        int rank;
        MPI_Comm_rank(comm, &rank);
        std::cout << ol << " " << rank << " | ";
        for (auto&& u : up) {
            for (auto&& y : u.first) {
                std::cout << y << " ";
            }
            std::cout << " :: ";
        }
        std::cout << std::endl;

            std::vector<int> counts(procs, 0);
            auto plex2rank = getPlex2Rank<D - 1u>();
            for (auto&& u : up) {
                ++counts[plex2rank(u.first)];
            }
            std::vector<std::ptrdiff_t> offsets(procs + 1);
            offsets[0] = 0;
            std::partial_sum(counts.begin(), counts.end(), offsets.begin() + 1);

            std::vector<Simplex<D - 1u>> boundaryFaces(offsets.back());
            std::vector<Simplex<D>> boundaryElems(offsets.back());
            for (auto&& u : up) {
                auto rank = plex2rank(u.first);
                boundaryFaces[offsets[rank]] = u.first;
                boundaryElems[offsets[rank]] = elems[u.second];
                ++offsets[rank];
            }

            // Exchange boundary faces and elements
            AllToAllV a2a(counts, comm);
            mpi_array_type<uint64_t> mpi_facet_t(D);
            mpi_array_type<uint64_t> mpi_elem_t(D + 1u);
            auto requestedBoundaryFaces = a2a.exchange(boundaryFaces, mpi_facet_t.get());
            auto requestedBoundaryElems = a2a.exchange(boundaryElems, mpi_elem_t.get());
            a2a.swap();

            std::unordered_multimap<Simplex<D - 1u>, Simplex<D>, SimplexHash<D - 1u>> requestedUp;
            for (std::size_t i = 0; i < requestedBoundaryFaces.size(); ++i) {
                requestedUp.emplace(requestedBoundaryFaces[i], requestedBoundaryElems[i]);
            }

            for (std::size_t i = 0; i < requestedBoundaryElems.size(); ++i) {
                auto range = requestedUp.equal_range(requestedBoundaryFaces[i]);
                while (range.first != range.second &&
                       range.first->second == requestedBoundaryElems[i]) {
                    ++range.first;
                }
                assert(range.first != range.second);
                requestedBoundaryElems[i] = range.first->second;
            }
            boundaryElems = a2a.exchange(requestedBoundaryElems, mpi_elem_t.get());

            // Remove duplicates
            std::sort(boundaryElems.begin(), boundaryElems.end());
            boundaryElems.erase(std::unique(boundaryElems.begin(), boundaryElems.end()),
                                boundaryElems.end());

            elems.reserve(elems.size() + boundaryElems.size());
            elems.insert(elems.end(), boundaryElems.begin(), boundaryElems.end());
        }
        return elems;
    }

    auto getBoundaryFaces(std::vector<Simplex<D>> const& elems) const {
        // Construct upward map from faces to local element ids
        std::unordered_multimap<Simplex<D - 1u>, std::size_t, SimplexHash<D - 1u>> up;
        for (std::size_t elNo = 0; elNo < elems.size(); ++elNo) {
            auto downward = elems[elNo].template downward<D - 1u>();
            for (auto& s : downward) {
                up.emplace(s, elNo);
            }
        }
        // Delete all internal faces and count number of faces per rank
        auto const deleteInternalFaces = [](auto& up) {
            for (auto it = up.begin(); it != up.end();) {
                auto count = up.count(it->first);
                assert(count <= 2);
                if (count > 1) {
                    auto range = up.equal_range(it->first);
                    it = up.erase(range.first, range.second);
                } else {
                    ++it;
                }
            }
        };
        //int rank;
        //MPI_Comm_rank(comm, &rank);
        //std::cout << rank << " | ";
        //for (auto&& u : up) {
            //for (auto&& y : u.first) {
                //std::cout << y << " ";
            //}
            //std::cout << " :: ";
        //}
        //std::cout << std::endl;
        deleteInternalFaces(up);
        //std::cout << rank << " | ";
        //for (auto&& u : up) {
            //for (auto&& y : u.first) {
                //std::cout << y << " ";
            //}
            //std::cout << " || ";
        //}
        //std::cout << std::endl;
        return up;
    }

    auto deleteDomainBoundaryFaces(
        std::unordered_multimap<Simplex<D - 1u>, std::size_t, SimplexHash<D - 1u>>& up) const {
        int procs;
        MPI_Comm_size(comm, &procs);
        auto myComm = comm;
        auto plex2rank = getPlex2Rank<D - 1u>();
        auto const figureOutWhichFacesAppearTwiceInDistributedMemory = [&procs, &myComm,
                                                                        &plex2rank](auto& up) {
            std::vector<int> counts(procs, 0);
            for (auto&& u : up) {
                ++counts[plex2rank(u.first)];
            }
            // Send domain and partition boundary faces to face owner
            std::vector<std::ptrdiff_t> offsets(procs + 1);
            offsets[0] = 0;
            std::partial_sum(counts.begin(), counts.end(), offsets.begin() + 1);
            auto faces = std::vector<Simplex<D - 1u>>(offsets.back());
            for (auto&& u : up) {
                auto rank = plex2rank(u.first);
                faces[offsets[rank]++] = u.first;
            }
            AllToAllV a2a(counts, myComm);
            mpi_array_type<uint64_t> mpi_plex_t(D);
            auto requestedFaces = a2a.exchange(faces, mpi_plex_t.get());
            auto requestedFacesAsMultiset =
                std::unordered_multiset<Simplex<D - 1u>, SimplexHash<D - 1u>>(
                    requestedFaces.begin(), requestedFaces.end());
            auto requestedFaceCount = std::vector<std::size_t>(requestedFaces.size());
            for (std::size_t fNo = 0; fNo < requestedFaces.size(); ++fNo) {
                requestedFaceCount[fNo] = requestedFacesAsMultiset.count(requestedFaces[fNo]);
            }
            a2a.swap();
            auto faceCount = a2a.exchange(requestedFaceCount);
            return std::make_pair(faces, faceCount);
        };

        auto [faces, faceCount] = figureOutWhichFacesAppearTwiceInDistributedMemory(up);

        std::unordered_set<Simplex<D - 1u>, SimplexHash<D - 1u>> domainBoundaryFaces;
        assert(faceCount.size() == faces.size());
        for (std::size_t fNo = 0; fNo < faceCount.size(); ++fNo) {
            assert(1 <= faceCount[fNo] && faceCount[fNo] <= 2);
            if (faceCount[fNo] == 1) {
                assert(up.find(faces[fNo]) != up.end());
                up.erase(faces[fNo]);
                domainBoundaryFaces.insert(faces[fNo]);
            }
        }
        return domainBoundaryFaces;
    }

    template <std::size_t DD> auto getPlex2Rank() const {
        int procs;
        MPI_Comm_size(comm, &procs);
        if constexpr (DD == 0) {
            if (vtxdist.size() > 0) {
                SortedDistributionToRank v2r(vtxdist);
                return std::function([v2r](Simplex<0> const& plex) -> int { return v2r(plex[0]); });
            }
            return std::function(
                [procs](Simplex<0> const& plex) -> int { return plex[0] % procs; });
        } else {
            return [procs](Simplex<DD> const& plex) { return SimplexHash<DD>()(plex) % procs; };
        }
    }

    std::size_t getVertexLID(Simplex<0> const& plex) const {
        int rank;
        MPI_Comm_rank(comm, &rank);
        assert(plex[0] >= vtxdist[rank] && plex[0] < vtxdist[rank + 1]);
        return plex[0] - vtxdist[rank];
    }

    template <std::size_t DD> auto getFaces(std::vector<Simplex<D>> const& elems) const {
        auto plex2rank = getPlex2Rank<DD>();

        int procs;
        MPI_Comm_size(comm, &procs);

        std::vector<std::set<Simplex<DD>>> requiredFaces(procs);
        for (auto& elem : elems) {
            auto downward = elem.template downward<DD>();
            for (auto& s : downward) {
                requiredFaces[plex2rank(s)].insert(s);
            }
        }
        std::vector<int> counts(procs, 0);
        std::size_t total = 0;
        for (int p = 0; p < procs; ++p) {
            auto size = requiredFaces[p].size();
            counts[p] = size;
            total += size;
        }

        std::vector<Simplex<DD>> faces;
        faces.reserve(total);
        for (auto& perRank : requiredFaces) {
            std::copy(perRank.begin(), perRank.end(), std::back_inserter(faces));
        }

        // Exchange data
        AllToAllV a2a(counts, comm);
        mpi_array_type<uint64_t> mpi_plex_t(DD + 1);
        auto requestedFaces = a2a.exchange(faces, mpi_plex_t.get());
        a2a.swap();

        auto lf = LocalFaces<DD>(std::move(faces), getContiguousGIDs(requestedFaces, a2a));
        if constexpr (DD == 0) {
            if (vertexData) {
                std::vector<std::size_t> lids;
                lids.reserve(requestedFaces.size());
                for (auto& face : requestedFaces) {
                    lids.emplace_back(getVertexLID(face));
                }
                lf.setMeshData(vertexData->redistributed(lids, a2a));
            }
        } else if constexpr (0 < DD && DD < D) {
            auto& boundaryMesh = std::get<DD>(boundaryMeshes);
            if (boundaryMesh && boundaryMesh->elementData) {
                boundaryMesh->repartitionByHash();
                auto map = boundaryMesh->makeG2LMap();
                std::vector<std::size_t> lids;
                lids.reserve(requestedFaces.size());
                for (auto& face : requestedFaces) {
                    auto it = map.find(face);
                    if (it == map.end()) {
                        lids.emplace_back(std::numeric_limits<std::size_t>::max());
                    } else {
                        lids.emplace_back(it->second);
                    }
                }
                lf.setMeshData(boundaryMesh->elementData->redistributed(lids, a2a));
            }
        }

        getSharedRanks(lf, requestedFaces, a2a);
        return lf;
    }

    template <std::size_t DD>
    std::vector<std::size_t> getContiguousGIDs(std::vector<Simplex<DD>> const& requestedFaces,
                                               AllToAllV const& a2a) const {
        std::vector<std::size_t> cGIDs;
        cGIDs.reserve(requestedFaces.size());
        if constexpr (DD == 0) {
            for (auto& face : requestedFaces) {
                cGIDs.emplace_back(face[0]);
            }
        } else {
            int rank;
            MPI_Comm_rank(comm, &rank);

            std::map<Simplex<DD>, std::size_t> ownedFacesToCGID;
            for (auto& face : requestedFaces) {
                ownedFacesToCGID[face] = -1;
            }

            std::size_t ownedSize = ownedFacesToCGID.size();
            std::size_t gidOffset;
            MPI_Scan(&ownedSize, &gidOffset, 1, mpi_type_t<std::size_t>(), MPI_SUM, comm);
            gidOffset -= ownedSize;

            std::size_t cGID = gidOffset;
            for (auto& faceToGID : ownedFacesToCGID) {
                faceToGID.second = cGID++;
            }

            for (auto& face : requestedFaces) {
                cGIDs.emplace_back(ownedFacesToCGID[face]);
            }
        }
        assert(requestedFaces.size() == cGIDs.size());

        return a2a.exchange(cGIDs);
    }

    template <std::size_t DD>
    void getSharedRanks(LocalFaces<DD>& lf, std::vector<Simplex<DD>> const& requestedFaces,
                        AllToAllV const& a2a) const {
        int procs;
        MPI_Comm_size(comm, &procs);

        std::unordered_map<Simplex<DD>, std::vector<int>, SimplexHash<DD>> sharedRanksInfo;
        for (auto [p, i] : a2a.getSDispls()) {
            sharedRanksInfo[requestedFaces[i]].emplace_back(p);
        }

        std::vector<int> sharedRanksSendCount;
        sharedRanksSendCount.reserve(requestedFaces.size());
        std::size_t totalSharedRanksSendCount = 0;
        for (auto& face : requestedFaces) {
            sharedRanksSendCount.emplace_back(sharedRanksInfo[face].size());
            totalSharedRanksSendCount += sharedRanksInfo[face].size();
        }

        auto sharedRanksRecvCount = a2a.exchange(sharedRanksSendCount);

        std::vector<int> requestedSharedRanks;
        requestedSharedRanks.reserve(totalSharedRanksSendCount);
        for (auto& face : requestedFaces) {
            std::copy(sharedRanksInfo[face].begin(), sharedRanksInfo[face].end(),
                      std::back_inserter(requestedSharedRanks));
        }
        std::vector<int> sendcounts(procs, 0);
        std::vector<int> recvcounts(procs, 0);
        for (auto [p, i] : a2a.getSDispls()) {
            sendcounts[p] += sharedRanksSendCount[i];
        }
        for (auto [p, i] : a2a.getRDispls()) {
            recvcounts[p] += sharedRanksRecvCount[i];
        }

        AllToAllV a2aSharedRanks(std::move(sendcounts), std::move(recvcounts));
        auto sharedRanks = a2aSharedRanks.exchange(requestedSharedRanks);
        Displacements sharedRanksDispls(sharedRanksRecvCount);

        lf.setSharedRanks(std::move(sharedRanks), std::move(sharedRanksDispls));
    }

    std::vector<simplex_t> elems_;
    std::unique_ptr<MeshData> vertexData;
    std::unique_ptr<MeshData> elementData;
    MPI_Comm comm;
    bool isPartitionedByHash = false;
    std::vector<std::size_t> vtxdist;
    ntuple_t<global_mesh_ptr, D> boundaryMeshes;
};
} // namespace tndm

#endif // GLOBALSIMPLEXMESH_H
