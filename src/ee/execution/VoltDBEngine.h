/* This file is part of VoltDB.
 * Copyright (C) 2008-2012 VoltDB Inc.
 *
 * This file contains original code and/or modifications of original code.
 * Any modifications made by VoltDB Inc. are licensed under the following
 * terms and conditions:
 *
 * VoltDB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * VoltDB is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with VoltDB.  If not, see <http://www.gnu.org/licenses/>.
 */
/* Copyright (C) 2008 by H-Store Project
 * Brown University
 * Massachusetts Institute of Technology
 * Yale University
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef VOLTDBENGINE_H
#define VOLTDBENGINE_H

#include <map>
#include <set>
#include <string>
#include <vector>
#include <cassert>

#include "boost/shared_ptr.hpp"
#include "json_spirit/json_spirit.h"
#include "catalog/database.h"
#include "common/ids.h"
#include "common/serializeio.h"
#include "common/types.h"
#include "common/valuevector.h"
#include "common/Pool.hpp"
#include "common/UndoLog.h"
#include "common/DummyUndoQuantum.hpp"
#include "common/SerializableEEException.h"
#include "common/DefaultTupleSerializer.h"
#include "execution/FragmentManager.h"
#include "plannodes/plannodefragment.h"
#include "stats/StatsAgent.h"
#include "storage/TempTableLimits.h"
#include "common/ThreadLocalPool.h"

#define MAX_BATCH_COUNT 1000
#define MAX_PARAM_COUNT 1000 // or whatever

namespace boost {
template <typename T> class shared_ptr;
}

namespace catalog {
class Catalog;
class PlanFragment;
class Table;
class Statement;
class Cluster;
}

class VoltDBIPC;

namespace voltdb {

class AbstractExecutor;
class AbstractPlanNode;
class SerializeInput;
class SerializeOutput;
class Table;
class Topend;
class CatalogDelegate;
class ReferenceSerializeInput;
class ReferenceSerializeOutput;
class PlanNodeFragment;
class ExecutorContext;
class RecoveryProtoMsg;

const int64_t DEFAULT_TEMP_TABLE_MEMORY = 1024 * 1024 * 100;

/**
 * Represents an Execution Engine which holds catalog objects (i.e. table) and executes
 * plans on the objects. Every operation starts from this object.
 * This class is designed to be single-threaded.
 */
// TODO(evanj): Used by JNI so must be exported. Remove when we only one .so
class __attribute__((visibility("default"))) VoltDBEngine {
    public:
        /** Constructor for test code: this does not enable JNI callbacks. */
        VoltDBEngine() :
          m_dummyUndoQuantum(),
          m_currentUndoQuantum(&m_dummyUndoQuantum),
          m_staticParams(MAX_PARAM_COUNT),
          m_currentOutputDepId(-1),
          m_currentInputDepId(-1),
          m_numResultDependencies(0)
        {}

        VoltDBEngine(Topend *topend);
        bool initialize(int32_t clusterIndex,
                        int64_t siteId,
                        int32_t partitionId,
                        int32_t hostId,
                        std::string hostname,
                        int64_t tempTableMemoryLimit,
                        int32_t totalPartitions);
        virtual ~VoltDBEngine();

        inline int32_t getClusterIndex() const { return m_clusterIndex; }
        inline int64_t getSiteId() const { return m_siteId; }

        // ------------------------------------------------------------------
        // OBJECT ACCESS FUNCTIONS
        // ------------------------------------------------------------------
        catalog::Catalog *getCatalog() const;

        Table* getTable(int32_t tableId) const;
        Table* getTable(std::string name) const;
        // Serializes table_id to out. Throws if unsuccessful.
        void serializeTable(int32_t tableId, SerializeOutput* out) const;

        // -------------------------------------------------
        // Execution Functions
        // -------------------------------------------------

        /**
         * Utility used for deserializing ParameterSet passed from Java.
         */
        void deserializeParameterSet(const char *parameter_buffer, int parameter_buffer_capacity);
        void deserializeParameterSet();

        int executeQuery(int64_t planfragmentId, int32_t outputDependencyId, int32_t inputDependencyId,
                         int64_t txnId, int64_t lastCommittedTxnId, bool first, bool last);

        // ensure a plan fragment is loaded, given a graph
        // return the fragid and cache statistics
        int loadFragment(const char *plan, int32_t length, int64_t &fragId, bool &wasHit, int64_t &cacheSize);
        // purge cached plans over the specified cache size
        void resizePlanCache();

        // -------------------------------------------------
        // Dependency Transfer Functions
        // -------------------------------------------------
        bool send(Table* dependency);
        int loadNextDependency(Table* destination);

        // -------------------------------------------------
        // Catalog Functions
        // -------------------------------------------------
        bool loadCatalog(const int64_t txnId, const std::string &catalogPayload);
        bool updateCatalog(const int64_t txnId, const std::string &catalogPayload);
        bool processCatalogAdditions(bool addAll, int64_t txnId);
        bool processCatalogDeletes(int64_t txnId);
        bool rebuildPlanFragmentCollections();
        bool rebuildTableCollections();


        /**
        * Load table data into a persistent table specified by the tableId parameter.
        * This must be called at most only once before any data is loaded in to the table.
        */
        bool loadTable(int32_t tableId,
                       ReferenceSerializeInput &serializeIn,
                       int64_t txnId, int64_t lastCommittedTxnId);

        void resetReusedResultOutputBuffer() {
            m_resultOutput.initializeWithPosition(m_reusedResultBuffer, m_reusedResultCapacity, 0);
            m_exceptionOutput.initializeWithPosition(m_exceptionBuffer, m_exceptionBufferCapacity, 0);
            *reinterpret_cast<int32_t*>(m_exceptionBuffer) = voltdb::VOLT_EE_EXCEPTION_TYPE_NONE;
        }


        inline ReferenceSerializeOutput& getResultOutputSerializer() { return m_resultOutput; }
        inline ReferenceSerializeOutput& getExceptionOutputSerializer() { return m_exceptionOutput; }
        void setBuffers(char *resultBuffer, int resultBufferCapacity,
                        char *exceptionBuffer, int exceptionBufferCapacity)
        {
            m_reusedResultBuffer = resultBuffer;
            m_reusedResultCapacity = resultBufferCapacity;
            m_exceptionBuffer = exceptionBuffer;
            m_exceptionBufferCapacity = exceptionBufferCapacity;
        }


        /**
         * Retrieves the size in bytes of the data that has been placed in the reused result buffer
         */
        int getResultsSize() const;

        /** Returns the buffer for receiving result tables from EE. */
        inline char* getReusedResultBuffer() const { return m_reusedResultBuffer;}
        /** Returns the size of buffer for receiving result tables from EE. */
        inline int getReusedResultBufferCapacity() const { return m_reusedResultCapacity;}

        int hashinate(int32_t partitionCount);

        int64_t* getBatchFragmentIdsContainer() { return m_batchFragmentIdsContainer; }

        /** check if this value hashes to the local partition */
        bool isLocalSite(const NValue& value);

        // -------------------------------------------------
        // Non-transactional work methods
        // -------------------------------------------------

        /** Perform once per second, non-transactional work. */
        void tick(int64_t timeInMillis, int64_t lastCommittedTxnId);

        /** flush active work (like EL buffers) */
        void quiesce(int64_t lastCommittedTxnId);

        // -------------------------------------------------
        // Save and Restore Table to/from disk functions
        // -------------------------------------------------

        /**
         * Save the table specified by catalog id tableId to the
         * absolute path saveFilePath
         *
         * @param tableGuid the GUID of the table in the catalog
         * @param saveFilePath the full path of the desired savefile
         * @return true if successful, false if save failed
         */
        bool saveTableToDisk(int32_t clusterId, int32_t databaseId, int32_t tableId, std::string saveFilePath);

        /**
         * Restore the table from the absolute path saveFilePath
         *
         * @param restoreFilePath the full path of the file with the
         * table to restore
         * @return true if successful, false if save failed
         */
        bool restoreTableFromDisk(std::string restoreFilePath);

        // -------------------------------------------------
        // Debug functions
        // -------------------------------------------------
        std::string debug(void) const;

        /** Counts tuples modified by a plan fragment */
        int64_t m_tuplesModified;
        /** True if any fragments in a batch have modified any tuples */
        bool m_dirtyFragmentBatch;

        std::string m_stmtName;
        std::string m_fragName;

        std::map<std::string, int*> m_indexUsage;

        // -------------------------------------------------
        // Statistics functions
        // -------------------------------------------------
        voltdb::StatsAgent& getStatsManager();

        /**
         * Retrieve a set of statistics and place them into the result buffer as a set of VoltTables.
         * @param selector StatisticsSelectorType indicating what set of statistics should be retrieved
         * @param locators Integer identifiers specifying what subset of possible statistical sources should be polled. Probably a CatalogId
         *                 Can be NULL in which case all possible sources for the selector should be included.
         * @param numLocators Size of locators array.
         * @param interval Whether to return counters since the beginning or since the last time this was called
         * @param Timestamp to embed in each row
         * @return Number of result tables, 0 on no results, -1 on failure.
         */
        int getStats(
                int selector,
                int locators[],
                int numLocators,
                bool interval,
                int64_t now);

        inline void purgeStringPool() { m_stringPool.purge(); }

        inline void setUndoToken(int64_t nextUndoToken) {
            if (nextUndoToken == INT64_MAX) {
                return;
            }
            if (m_currentUndoQuantum != NULL && m_currentUndoQuantum != &m_dummyUndoQuantum) {
                assert(nextUndoToken >= m_currentUndoQuantum->getUndoToken());
                if (m_currentUndoQuantum->getUndoToken() == nextUndoToken) {
                    return;
                }
            }
            setUndoQuantum(m_undoLog.generateUndoQuantum(nextUndoToken));
        }

        inline void releaseUndoToken(int64_t undoToken) {
            if (m_currentUndoQuantum == &m_dummyUndoQuantum) {
                return;
            }
            if (m_currentUndoQuantum != NULL && m_currentUndoQuantum->getUndoToken() == undoToken) {
                m_currentUndoQuantum = NULL;
            }
            m_undoLog.release(undoToken);
        }

        inline void undoUndoToken(int64_t undoToken) {
            if (m_currentUndoQuantum == &m_dummyUndoQuantum) {
                return;
            }
            m_undoLog.undo(undoToken);
            m_currentUndoQuantum = NULL;
        }

        inline Topend* getTopend() { return m_topend; }

        /**
         * Activate a table stream of the specified type for the specified table.
         * Returns true on success and false on failure
         */
        bool activateTableStream(const CatalogId tableId, const TableStreamType streamType);

        /**
         * Serialize more tuples from the specified table that has an active stream of the specified type
         * Returns the number of bytes worth of tuple data serialized or 0 if there are no more.
         * Returns -1 if the table is not in COW mode. The table continues to be in COW (although no copies are made)
         * after all tuples have been serialize until the last call to cowSerializeMore which returns 0 (and deletes
         * the COW context). Further calls will return -1
         */
        int tableStreamSerializeMore(
                ReferenceSerializeOutput *out,
                CatalogId tableId,
                const TableStreamType streamType);

        /*
         * Apply the updates in a recovery message.
         */
        void processRecoveryMessage(RecoveryProtoMsg *message);

        /**
         * Perform an action on behalf of Export.
         *
         * @param if syncAction is true, the stream offset being set for a table
         * @param the catalog version qualified id of the table to which this action applies
         * @return the universal offset for any poll results (results
         * returned separatedly via QueryResults buffer)
         */
        int64_t exportAction(bool syncAction, int64_t ackOffset, int64_t seqNo, std::string tableSignature);

        void getUSOForExportTable(size_t &ackOffset, int64_t &seqNo, std::string tableSignature);

        /**
         * Retrieve a hash code for the specified table
         */
        size_t tableHashCode(int32_t tableId);

    private:
        std::string getClusterNameFromTable(voltdb::Table *table);
        std::string getDatabaseNameFromTable(voltdb::Table *table);

        // -------------------------------------------------
        // Initialization Functions
        // -------------------------------------------------
        bool initPlanFragment(const int64_t fragId, const std::string planNodeTree);
        bool initPlanNode(const int64_t fragId,
                          AbstractPlanNode* node,
                          TempTableLimits* limits);
        bool initCluster();
        bool initMaterializedViews(bool addAll);
        bool updateCatalogDatabaseReference();

        void printReport();

        void setUndoQuantum(voltdb::UndoQuantum *undoQuantum);

        // -------------------------------------------------
        // Data Members
        // -------------------------------------------------

        /**
         * Keep a list of executors for runtime - intentionally near the top of VoltDBEngine
         */
        struct ExecutorVector {
            ExecutorVector(int64_t logThreshold,
                           int64_t memoryLimit,
                           PlanNodeFragment *fragment) : planFragment(fragment)
            {
                limits.setLogThreshold(logThreshold);
                limits.setMemoryLimit(memoryLimit);
            }
            boost::shared_ptr<PlanNodeFragment> planFragment;
            std::vector<AbstractExecutor*> list;
            TempTableLimits limits;
        };
        std::map<int64_t, boost::shared_ptr<ExecutorVector> > m_executorMap;

        voltdb::DummyUndoQuantum m_dummyUndoQuantum;
        voltdb::UndoLog m_undoLog;
        voltdb::UndoQuantum *m_currentUndoQuantum;

        int64_t m_siteId;
        int32_t m_partitionId;
        int32_t m_clusterIndex;
        int m_totalPartitions;
        size_t m_startOfResultBuffer;
        int64_t m_tempTableMemoryLimit;

        /*
         * Catalog delegates hashed by path.
         */
        std::map<std::string, CatalogDelegate*> m_catalogDelegates;

        // map catalog table id to table pointers
        std::map<int32_t, Table*> m_tables;

        // map catalog table name to table pointers
        std::map<std::string, Table*> m_tablesByName;

        /*
         * Map of catalog table ids to snapshotting tables.
         * Note that these tableIds are the ids when the snapshot
         * was initiated. The snapshot processor in Java does not
         * update tableIds when the catalog changes. The point of
         * reference, therefore, is consistently the catalog at
         * the point of snapshot initiation. It is always invalid
         * to try to map this tableId back to catalog::Table via
         * the catalog, at least w/o comparing table names.
         */
        std::map<int32_t, Table*> m_snapshottingTables;

        /*
         * Map of table signatures to exporting tables.
         */
        std::map<std::string, Table*> m_exportingTables;

        /**
         * System Catalog.
         */
        boost::shared_ptr<catalog::Catalog> m_catalog;
        catalog::Database *m_database;

        /** reused parameter container. */
        NValueArray m_staticParams;

        int m_usedParamcnt;

        /** buffer object for result tables. set when the result table is sent out to localsite. */
        FallbackSerializeOutput m_resultOutput;

        /** buffer object for exceptions generated by the EE **/
        ReferenceSerializeOutput m_exceptionOutput;

        char *m_exceptionBuffer;

        int m_exceptionBufferCapacity;

        /** buffer object to receive result tables from EE. */
        char* m_reusedResultBuffer;
        /** size of reused_result_buffer. */
        int m_reusedResultCapacity;

        int64_t m_batchFragmentIdsContainer[MAX_BATCH_COUNT];

        /** number of plan fragments executed so far */
        int m_pfCount;

        // used for sending and recieving deps
        // set by the executeQuery / executeFrag type methods
        int m_currentOutputDepId;
        int m_currentInputDepId;

        /** Stats manager for this execution engine **/
        voltdb::StatsAgent m_statsManager;

        /*
         * Pool for short lived strings that will not live past the return back to Java.
         */
        Pool m_stringPool;

        /*
         * When executing a plan fragment this is set to the number of result dependencies
         * that have been serialized into the m_resultOutput
         */
        int32_t m_numResultDependencies;

        char *m_templateSingleLongTable;

        const static int m_templateSingleLongTableSize
          = 4 // depid
          + 4 // table size
          + 1 // status code
          + 4 // header size
          + 2 // column count
          + 1 // column type
          + 4 + 15 // column name (length + modified_tuples)
          + 4 // tuple count
          + 4 // first row size
          + 8;// modified tuples

        Topend *m_topend;

        // For data from engine that must be shared/distributed to
        // other components. (Components MUST NOT depend on VoltDBEngine.h).
        ExecutorContext *m_executorContext;

        DefaultTupleSerializer m_tupleSerializer;

        FragmentManager m_fragmentManager;

    private:
        ThreadLocalPool m_tlPool;
};

} // namespace voltdb

#endif // VOLTDBENGINE_H
