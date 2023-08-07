/*
 * Copyright (2022) Bytedance Ltd. and/or its affiliates
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <Catalog/Catalog.h>
#include <Common/Status.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeEnum.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <Interpreters/Context.h>
#include <Parsers/ASTSelectQuery.h>
#include <CloudServices/CnchPartsHelper.h>
#include <Storages/System/StorageSystemCnchParts.h>
#include <Storages/StorageCnchMergeTree.h>
#include <Storages/VirtualColumnUtils.h>
#include <Transaction/ICnchTransaction.h>
#include <Transaction/TransactionCoordinatorRcCnch.h>
#include <Storages/System/CollectWhereClausePredicate.h>
#include <TSO/TSOClient.h>
#include <fmt/format.h>
#include <map>

namespace DB
{
namespace ErrorCodes
{
    extern const int INCORRECT_QUERY;
}
NamesAndTypesList StorageSystemCnchParts::getNamesAndTypes()
{
    auto type_enum = std::make_shared<DataTypeEnum8>(DataTypeEnum8::Values{
        {"VisiblePart", static_cast<Int8>(PartType::VisiblePart)},
        {"InvisiblePart", static_cast<Int8>(PartType::InvisiblePart)},
        {"DropRange", static_cast<Int8>(PartType::DropRange)},
        {"DroppedPart", static_cast<Int8>(PartType::DroppedPart)},
    });

    return {
        {"database", std::make_shared<DataTypeString>()},
        {"table", std::make_shared<DataTypeString>()},
        {"table_uuid", std::make_shared<DataTypeUUID>()},
        {"partition", std::make_shared<DataTypeString>()},
        {"name", std::make_shared<DataTypeString>()},
        {"bytes_on_disk", std::make_shared<DataTypeUInt64>()},
        {"rows_count", std::make_shared<DataTypeUInt64>()},
        {"columns", std::make_shared<DataTypeString>()},
        {"marks_count", std::make_shared<DataTypeUInt64>()},
        {"index_granularity", std::make_shared<DataTypeArray>(std::make_shared<DataTypeUInt64>())},
        {"commit_time", std::make_shared<DataTypeDateTime>()},
        {"kv_commit_time", std::make_shared<DataTypeDateTime>()},
        {"columns_commit_time", std::make_shared<DataTypeDateTime>()},
        {"mutation_commit_time", std::make_shared<DataTypeDateTime>()},
        {"previous_version", std::make_shared<DataTypeUInt64>()},
        {"partition_id", std::make_shared<DataTypeString>()},
        {"bucket_number", std::make_shared<DataTypeInt64>()},
        {"table_definition_hash", std::make_shared<DataTypeUInt64>()},
        {"outdated", std::make_shared<DataTypeUInt8>()},
        {"visible", std::make_shared<DataTypeUInt8>()},
        {"part_type", std::move(type_enum)},
    };
}

/**
 * What's the difference between outdated, visible and part_type.
 *  1. outdated: parts which should be deleted by GCThread.
 *  2. visible: parts is visible in select query or some background tasks.
 *     Note: parts which not visible, for example, partial parts, doesn't means it is outdated.
 *  3. part_type:
 *     ----------------------------------------------------------------------------------------------------------------
 *       type:       | visible | outdated |        description
 *     ----------------------------------------------------------------------------------------------------------------
 *     VisiblePart   |   1     |   0      | visible part; generated by INSERT
 *     InVisiblePart |   0     |   0      | covered by other part, but didn't should be deleted; generated by MutateTask
 *     DropPart      |   0     |   1      | part->deleted() = true; generated by MergeTask
 *     DropRange     |   0     |   1      | level = MAX_LEVEL; generated by DROP PARTITION command
 *     -----------------------------------------------------------------------------------------------------------------
 */

NamesAndAliases StorageSystemCnchParts::getNamesAndAliases()
{
    return
    {
        {"active", {std::make_shared<DataTypeUInt8>()}, "visible"},
        {"bytes", {std::make_shared<DataTypeUInt64>()}, "bytes_on_disk"},
        {"rows", {std::make_shared<DataTypeUInt64>()}, "rows_count"}
    };
}

static std::vector<std::pair<String, String>> filterTables(const ContextPtr & context, const SelectQueryInfo & query_info)
{
    if (!context->getSettingsRef().enable_multiple_tables_for_cnch_parts)
        throw Exception("You should specify database and table in where cluster or set enable_multiple_tables_for_cnch_parts to enable visit multiple tables",
            ErrorCodes::LOGICAL_ERROR);

    auto catalog = context->getCnchCatalog();
    auto table_models = catalog->getAllTables();

    Block block_to_filter;

    MutableColumnPtr database_column = ColumnString::create();
    MutableColumnPtr table_name_column = ColumnString::create();
    MutableColumnPtr table_uuid_column = ColumnUInt128::create();

    for (const auto & table_model: table_models)
    {
        if (Status::isDeleted(table_model.status()))
            continue;

        database_column->insert(table_model.database());
        table_name_column->insert(table_model.name());
        table_uuid_column->insert(RPCHelpers::createUUID(table_model.uuid()));
    }

    block_to_filter.insert(ColumnWithTypeAndName(std::move(database_column), std::make_shared<DataTypeString>(), "database"));
    block_to_filter.insert(ColumnWithTypeAndName(std::move(table_name_column), std::make_shared<DataTypeString>(), "table_name"));
    block_to_filter.insert(ColumnWithTypeAndName(std::move(table_uuid_column), std::make_shared<DataTypeUUID>(), "table_uuid"));

    VirtualColumnUtils::filterBlockWithQuery(query_info.query, block_to_filter, context);

    if (!block_to_filter.rows())
        return {};

    std::vector<std::pair<String, String>> res;

    auto database_column_res = block_to_filter.getByName("database").column;
    auto table_name_column_res = block_to_filter.getByName("table_name").column;
    for (size_t i = 0; i < database_column_res->size(); ++i)
        res.emplace_back((*database_column_res)[i].get<String>(), (*table_name_column_res)[i].get<String>());

    LOG_DEBUG(&Poco::Logger::get("SystemCnchParts"), "Got {} tables from catalog after filter", res.size());
    return res;
}

void StorageSystemCnchParts::fillData(MutableColumns & res_columns, ContextPtr context, const SelectQueryInfo & query_info) const
{
    auto cnch_catalog = context->getCnchCatalog();
    if (context->getServerType() != ServerType::cnch_server || !cnch_catalog)
        throw Exception("Table system.cnch_parts only support cnch_server", ErrorCodes::NOT_IMPLEMENTED);

    std::vector<std::pair<String, String>> tables;

    ASTPtr where_expression = query_info.query->as<ASTSelectQuery &>().where();

    const std::vector<std::map<String,String>> value_by_column_names = collectWhereORClausePredicate(where_expression, context);
    bool enable_filter_by_table = false;
    bool enable_filter_by_partition = false;
    String only_selected_db;
    String only_selected_table;
    String only_selected_partition_id;

    if (value_by_column_names.size() == 1)
    {
        const auto value_by_column_name = value_by_column_names.at(0);
        auto db_it = value_by_column_name.find("database");
        auto table_it = value_by_column_name.find("table");
        auto partition_it = value_by_column_name.find("partition_id");
        if ((db_it != value_by_column_name.end()) && (table_it != value_by_column_name.end()))
        {
            only_selected_db = db_it->second;
            only_selected_table = table_it->second;
            enable_filter_by_table = true;

            LOG_TRACE(&Poco::Logger::get("StorageSystemCnchParts"),
                    "filtering from catalog by table with db name {} and table name {}",
                    only_selected_db, only_selected_table);
        }

        if (partition_it != value_by_column_name.end())
        {
            only_selected_partition_id = partition_it->second;
            enable_filter_by_partition = true;

            LOG_TRACE(&Poco::Logger::get("StorageSystemCnchParts"),
                    "filtering from catalog by partition with partition name {}",
                    only_selected_partition_id);
        }
    }

    if (!(enable_filter_by_partition || enable_filter_by_table))
        LOG_TRACE(&Poco::Logger::get("StorageSystemCnchParts"), "No explicitly table and partition provided in where expression");

    // check for required structure of WHERE clause for cnch_parts
    if (!enable_filter_by_table)
    {
        tables = filterTables(context, query_info);
    }
    else
        tables.emplace_back(only_selected_db, only_selected_table);

    TransactionCnchPtr cnch_txn = context->getCurrentTransaction();
    TxnTimestamp start_time = cnch_txn ? cnch_txn->getStartTime() : TxnTimestamp{context->getTimestamp()};

    for (const auto & it : tables)
    {
        const String & database_name = it.first;
        const String & table_name = it.second;
        auto table = cnch_catalog->tryGetTable(*context, database_name, table_name, start_time);

        auto * cnch_merge_tree = dynamic_cast<StorageCnchMergeTree *>(table.get());
        if (!cnch_merge_tree)
        {
            if (enable_filter_by_table)
                throw Exception(
                    ErrorCodes::NOT_IMPLEMENTED,
                    "Table system.cnch_parts only support CnchMergeTree engine, but got `{}`",
                    table ? table->getName(): "unknown engine");
            else
                continue;
        }

        auto all_parts = enable_filter_by_partition
            ? cnch_catalog->getServerDataPartsInPartitions(table, {only_selected_partition_id}, start_time, nullptr)
            : cnch_catalog->getAllServerDataParts(table, start_time, nullptr);

        ServerDataPartsVector visible_alone_drop_ranges;
        ServerDataPartsVector invisible_dropped_parts;
        auto visible_parts = CnchPartsHelper::calcVisiblePartsForGC(all_parts, &visible_alone_drop_ranges, &invisible_dropped_parts);
        const FormatSettings format_settings;

        auto add_parts = [&](ServerDataPartPtr curr_part, PartType type, bool visible)
        {
            /// visible parts and all previous parts of visible parts should not be outdated.
            bool outdated = !visible;

            while (curr_part)
            {
                size_t col_num = 0;
                res_columns[col_num++]->insert(database_name);
                res_columns[col_num++]->insert(table_name);
                res_columns[col_num++]->insert(cnch_merge_tree->getStorageUUID());

                {
                    WriteBufferFromOwnString out;
                    curr_part->partition().serializeText(*cnch_merge_tree, out, format_settings);
                    res_columns[col_num++]->insert(out.str());
                }
                res_columns[col_num++]->insert(curr_part->name());
                res_columns[col_num++]->insert(curr_part->part_model().size());
                res_columns[col_num++]->insert(curr_part->part_model().rows_count());
                res_columns[col_num++]->insert(curr_part->part_model().columns());
                res_columns[col_num++]->insert(curr_part->part_model().marks_count());
                Array index_granularity;
                index_granularity.reserve(curr_part->part_model().index_granularities_size());
                for (const auto & granularity : curr_part->part_model().index_granularities())
                    index_granularity.push_back(granularity);
                res_columns[col_num++]->insert(index_granularity);

                res_columns[col_num++]->insert(TxnTimestamp(curr_part->getCommitTime()).toSecond());
                res_columns[col_num++]->insert(TxnTimestamp(curr_part->part_model().commit_time()).toSecond());
                res_columns[col_num++]->insert(TxnTimestamp(curr_part->part_model().columns_commit_time()).toSecond());
                res_columns[col_num++]->insert(TxnTimestamp(curr_part->part_model().mutation_commit_time()).toSecond());

                res_columns[col_num++]->insert(curr_part->info().hint_mutation);
                res_columns[col_num++]->insert(curr_part->info().partition_id);
                res_columns[col_num++]->insert(curr_part->part_model().bucket_number());
                res_columns[col_num++]->insert(curr_part->part_model().table_definition_hash());

                res_columns[col_num++]->insert(outdated);
                res_columns[col_num++]->insert(visible);

                /// The previous part of DropRange may be DroppedPart
                if (type == PartType::DropRange && curr_part->info().level != MergeTreePartInfo::MAX_LEVEL)
                    type = PartType::DroppedPart;

                res_columns[col_num++]->insert(static_cast<Int8>(type));

                curr_part = curr_part->tryGetPreviousPart();

                /// Invisible does not mean parts need to be deleted
                /// All previous part of visible parts is invisible.
                visible = false;

                if (type == PartType::VisiblePart)
                    type = PartType::InvisiblePart;
            }
        };

        for (auto & part: visible_parts)
        {
            if (part->deleted())
            {
                auto type = (part->info().level == MergeTreePartInfo::MAX_LEVEL) ? DropRange : DroppedPart;
                add_parts(part, type, false);
            }
            else
                add_parts(part, PartType::VisiblePart, true);
        }

        for (auto & part: visible_alone_drop_ranges)
            add_parts(part, PartType::DropRange, false);

        for (auto & part: invisible_dropped_parts)
            add_parts(part, PartType::DroppedPart, false);
    }
}

}
