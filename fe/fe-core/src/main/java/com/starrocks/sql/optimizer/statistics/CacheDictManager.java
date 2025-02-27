// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

package com.starrocks.sql.optimizer.statistics;

import com.github.benmanes.caffeine.cache.AsyncCacheLoader;
import com.github.benmanes.caffeine.cache.AsyncLoadingCache;
import com.github.benmanes.caffeine.cache.Caffeine;
import com.google.common.collect.ImmutableMap;
import com.google.common.collect.Sets;
import com.starrocks.catalog.Database;
import com.starrocks.common.Config;
import com.starrocks.common.Pair;
import com.starrocks.common.Status;
import com.starrocks.qe.ConnectContext;
import com.starrocks.server.GlobalStateMgr;
import com.starrocks.sql.optimizer.base.ColumnIdentifier;
import com.starrocks.thrift.TGlobalDict;
import com.starrocks.thrift.TStatisticData;
import org.apache.logging.log4j.LogManager;
import org.apache.logging.log4j.Logger;
import org.checkerframework.checker.nullness.qual.NonNull;

import java.util.List;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Executor;

import static com.starrocks.statistic.StatisticExecutor.queryDictSync;

public class CacheDictManager implements IDictManager {
    private static final Logger LOG = LogManager.getLogger(CacheDictManager.class);
    private static final Set<ColumnIdentifier> noDictStringColumns = Sets.newConcurrentHashSet();
    private static final Set<Long> forbiddenDictTableIds = Sets.newConcurrentHashSet();

    public static final Integer LOW_CARDINALITY_THRESHOLD = 255;

    private CacheDictManager() {
    }

    private static final CacheDictManager instance = new CacheDictManager();

    protected static CacheDictManager getInstance() {
        return instance;
    }

    private final AsyncCacheLoader<ColumnIdentifier, Optional<ColumnDict>> dictLoader =
            new AsyncCacheLoader<ColumnIdentifier, Optional<ColumnDict>>() {
                @Override
                public @NonNull
                CompletableFuture<Optional<ColumnDict>> asyncLoad(
                        @NonNull ColumnIdentifier columnIdentifier,
                        @NonNull Executor executor) {
                    return CompletableFuture.supplyAsync(() -> {
                        try {
                            long tableId = columnIdentifier.getTableId();
                            String columnName = columnIdentifier.getColumnName();
                            Pair<List<TStatisticData>, Status> result = queryDictSync(columnIdentifier.getDbId(),
                                    tableId, columnName);
                            if (result.second.isGlobalDictError()) {
                                LOG.debug("{}-{} isn't low cardinality string column", tableId, columnName);
                                noDictStringColumns.add(columnIdentifier);
                                return Optional.empty();
                            } else {
                                // check TStatisticData is not empty, There may be no such column Statistics in BE
                                if (!result.first.isEmpty()) {
                                    return deserializeColumnDict(tableId, columnName, result.first.get(0));
                                } else {
                                    return Optional.empty();
                                }
                            }

                        } catch (RuntimeException e) {
                            throw e;
                        } catch (Exception e) {
                            throw new CompletionException(e);
                        }
                    }, executor);
                }

                @Override
                public CompletableFuture<Optional<ColumnDict>> asyncReload(
                        @NonNull ColumnIdentifier key, @NonNull Optional<ColumnDict> oldValue,
                        @NonNull Executor executor) {
                    return asyncLoad(key, executor);
                }
            };

    private final AsyncLoadingCache<ColumnIdentifier, Optional<ColumnDict>> dictStatistics = Caffeine.newBuilder()
            .maximumSize(Config.statistic_cache_columns)
            .buildAsync(dictLoader);

    private Optional<ColumnDict> deserializeColumnDict(long tableId, String columnName, TStatisticData statisticData) {
        if (statisticData.dict == null) {
            throw new RuntimeException("Collect dict error in BE");
        }
        TGlobalDict tGlobalDict = statisticData.dict;
        ImmutableMap.Builder<String, Integer> dicts = ImmutableMap.builder();
        if (!tGlobalDict.isSetIds()) {
            return Optional.empty();
        }
        int dictSize = tGlobalDict.getIdsSize();
        ColumnIdentifier columnIdentifier = new ColumnIdentifier(tableId, columnName);
        if (dictSize > 256) {
            noDictStringColumns.add(columnIdentifier);
            return Optional.empty();
        } else {
            int dictDataSize = 0;
            for (int i = 0; i < dictSize; i++) {
                // a UTF-8 code may take up to 3 bytes
                dictDataSize += tGlobalDict.strings.get(i).length() * 3;
                // string offsets
                dictDataSize += 4;
            }
            // 1M
            final int DICT_PAGE_MAX_SIZE = 1024 * 1024;
            // If the dictionary data size exceeds 1M,
            // we won't use the global dictionary optimization.
            // In this case BE cannot guarantee that the dictionary page
            // will be generated after the compaction.
            // Additional 32 bytes reserved for security.
            if (dictDataSize > DICT_PAGE_MAX_SIZE - 32) {
                noDictStringColumns.add(columnIdentifier);
                return Optional.empty();
            }
        }
        for (int i = 0; i < dictSize; ++i) {
            dicts.put(tGlobalDict.strings.get(i), tGlobalDict.ids.get(i));
        }
        return Optional.of(new ColumnDict(dicts.build(), statisticData.meta_version));
    }

    @Override
    public boolean hasGlobalDict(long tableId, String columnName, long versionTime) {
        ColumnIdentifier columnIdentifier = new ColumnIdentifier(tableId, columnName);
        if (noDictStringColumns.contains(columnIdentifier)) {
            LOG.debug("{}-{} isn't low cardinality string column", tableId, columnName);
            return false;
        }

        if (forbiddenDictTableIds.contains(tableId)) {
            LOG.debug("table {} forbid low cardinality global dict", tableId);
            return false;
        }

        Set<Long> dbIds = ConnectContext.get().getCurrentSqlDbIds();
        for (Long id : dbIds) {
            Database db = GlobalStateMgr.getCurrentState().getDb(id);
            if (db != null && db.getTable(tableId) != null) {
                columnIdentifier.setDbId(db.getId());
                break;
            }
        }

        if (columnIdentifier.getDbId() == -1) {
            LOG.debug("{} couldn't find db id", columnName);
            return false;
        }

        CompletableFuture<Optional<ColumnDict>> result = dictStatistics.get(columnIdentifier);
        if (result.isDone()) {
            Optional<ColumnDict> realResult;
            try {
                realResult = result.get();
            } catch (Exception e) {
                LOG.warn(String.format("get dict cache for %d: %s failed", tableId, columnName), e);
                return false;
            }
            if (!realResult.isPresent()) {
                LOG.debug("Invalidate column {} dict cache because don't present", columnName);
                dictStatistics.synchronous().invalidate(columnIdentifier);
            } else if (realResult.get().getVersionTime() < versionTime) {
                LOG.debug("Invalidate column {} dict cache because out of date", columnName);
                dictStatistics.synchronous().invalidate(columnIdentifier);
            } else {
                return true;
            }
        }
        LOG.debug("{} first get column dict", columnName);
        return false;
    }

    @Override
    public boolean hasGlobalDict(long tableId, String columnName) {
        ColumnIdentifier columnIdentifier = new ColumnIdentifier(tableId, columnName);
        if (noDictStringColumns.contains(columnIdentifier)) {
            LOG.debug("{} isn't low cardinality string column", columnName);
            return false;
        }

        if (forbiddenDictTableIds.contains(tableId)) {
            LOG.debug("table {} forbid low cardinality global dict", tableId);
            return false;
        }

        return dictStatistics.asMap().containsKey(columnIdentifier);
    }

    @Override
    public void removeGlobalDict(long tableId, String columnName) {
        LOG.debug("remove dict for column {}", columnName);
        ColumnIdentifier columnIdentifier = new ColumnIdentifier(tableId, columnName);
        dictStatistics.synchronous().invalidate(columnIdentifier);
    }

    @Override
    public void forbidGlobalDict(long tableId) {
        LOG.debug("remove dict for table {}", tableId);
        forbiddenDictTableIds.add(tableId);
    }

    @Override
    public void updateGlobalDict(long tableId, String columnName, long versionTime) {
        ColumnIdentifier columnIdentifier = new ColumnIdentifier(tableId, columnName);
        if (!dictStatistics.asMap().containsKey(columnIdentifier)) {
            return;
        }

        CompletableFuture<Optional<ColumnDict>> columnFuture = dictStatistics.get(columnIdentifier);
        if (columnFuture.isDone()) {
            try {
                Optional<ColumnDict> columnOptional = columnFuture.get();
                if (columnOptional.isPresent()) {
                    ColumnDict columnDict = columnOptional.get();
                    ColumnDict newColumnDict = new ColumnDict(columnDict.getDict(), versionTime);
                    dictStatistics.put(columnIdentifier, CompletableFuture.completedFuture(Optional.of(newColumnDict)));
                    LOG.debug("update dict for column {}, version {}", columnName, versionTime);
                }
            } catch (Exception e) {
                LOG.warn(String.format("update dict cache for %d: %s failed", tableId, columnName), e);
            }
        }
    }

    @Override
    public Optional<ColumnDict> getGlobalDict(long tableId, String columnName) {
        ColumnIdentifier columnIdentifier = new ColumnIdentifier(tableId, columnName);
        CompletableFuture<Optional<ColumnDict>> columnFuture = dictStatistics.get(columnIdentifier);
        if (columnFuture.isDone()) {
            try {
                return columnFuture.get();
            } catch (Exception e) {
                LOG.warn(String.format("get dict cache for %d: %s failed", tableId, columnName), e);
            }
        }
        return Optional.empty();
    }
}
