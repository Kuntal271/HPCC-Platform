/*##############################################################################
    HPCC SYSTEMS software Copyright (C) 2022 HPCC Systems®.
    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at
       http://www.apache.org/licenses/LICENSE-2.0
    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include "parquetembed.hpp"
#include "arrow/result.h"
#include "parquet/arrow/schema.h"
#include "arrow/io/api.h"
#include <cmath>

#include "rtlembed.hpp"
#include "rtlds_imp.hpp"
#include "jfile.hpp"

static constexpr const char *MODULE_NAME = "parquet";
static constexpr const char *MODULE_DESCRIPTION = "Parquet Embed Helper";
static constexpr const char *VERSION = "Parquet Embed Helper 1.0.0";
static const char *COMPATIBLE_VERSIONS[] = {VERSION, nullptr};
static const NullFieldProcessor NULLFIELD(NULL);

/**
 * @brief Takes a pointer to an ECLPluginDefinitionBlock and passes in all the important info
 * about the plugin.
 */
extern "C" DECL_EXPORT bool getECLPluginDefinition(ECLPluginDefinitionBlock *pb)
{
    if (pb->size == sizeof(ECLPluginDefinitionBlockEx))
    {
        ECLPluginDefinitionBlockEx *pbx = (ECLPluginDefinitionBlockEx *)pb;
        pbx->compatibleVersions = COMPATIBLE_VERSIONS;
    }
    else if (pb->size != sizeof(ECLPluginDefinitionBlock))
        return false;

    pb->magicVersion = PLUGIN_VERSION;
    pb->version = VERSION;
    pb->moduleName = MODULE_NAME;
    pb->ECL = nullptr;
    pb->flags = PLUGIN_IMPLICIT_MODULE;
    pb->description = MODULE_DESCRIPTION;
    return true;
}

namespace parquetembed
{
static thread_local rapidjson::MemoryPoolAllocator<> jsonAlloc;

// //--------------------------------------------------------------------------
// Plugin Classes
//--------------------------------------------------------------------------

/**
 * @brief Throws an exception and gets called when an operation that is unsupported is attempted.
 *
 * @param feature Name of the feature that is currently unsupported.
 */
extern void UNSUPPORTED(const char *feature)
{
    throw MakeStringException(-1, "%s UNSUPPORTED feature: %s not supported in %s", MODULE_NAME, feature, VERSION);
}

/**
 * @brief Exits the program with a failure code and a message to display.
 *
 * @param message Message to display.
 * @param ... Takes any number of arguments that can be inserted into the string using %.
 */
extern void failx(const char *message, ...)
{
    va_list args;
    va_start(args, message);
    StringBuffer msg;
    msg.appendf("%s: ", MODULE_NAME).valist_appendf(message, args);
    va_end(args);
    rtlFail(0, msg.str());
}

/**
 * @brief Exits the program with a failure code and a message to display.
 *
 * @param message Message to display.
 */
extern void fail(const char *message)
{
    StringBuffer msg;
    msg.appendf("%s: ", MODULE_NAME).append(message);
    rtlFail(0, msg.str());
}

/**
 * @brief Contructs a ParquetReader for a specific file location.
 *
 * @param option The read or write option as well as information about partitioning.
 * @param _location The full path from which to read a Parquet file or partitioned dataset. Can be a filename or directory.
 * @param _maxRowCountInTable The number of rows in each batch when converting Parquet columns to rows.
 * @param _activityCtx Additional context about the thor workers running.
 */
ParquetReader::ParquetReader(const char *option, const char *_location, int _maxRowCountInTable, const char *_partitionFields, const IThorActivityContext *_activityCtx)
    : partOption(option), location(_location)
{
    maxRowCountInTable = _maxRowCountInTable;
    activityCtx = _activityCtx;
    pool = arrow::default_memory_pool();
    if (_partitionFields)
    {
        std::stringstream ss(_partitionFields);
        std::string field;
        while (std::getline(ss, field, ';'))
            partitionFields.push_back(field);
    }
}

ParquetReader::~ParquetReader()
{
    pool->ReleaseUnused();
}

/**
 * @brief Opens a read stream at the target location set in the constructor.
 *
 * @return Status object arrow::Status::OK if successful.
 */
arrow::Status ParquetReader::openReadFile()
{
    if (location.empty())
    {
        failx("Invalid option: The destination was not supplied.");
    }
    if (endsWithIgnoreCase(partOption.c_str(), "partition"))
    {
        // Create a filesystem
        std::shared_ptr<arrow::fs::FileSystem> fs;
        ARROW_ASSIGN_OR_RAISE(fs, arrow::fs::FileSystemFromUriOrPath(location));

        // FileSelector allows traversal of multi-file dataset
        arrow::fs::FileSelector selector;
        selector.base_dir = location; // The base directory to be searched is provided by the user in the location option.
        selector.recursive = true;    // Selector will search the base path recursively for partitioned files.

        // Create a file format
        std::shared_ptr<arrow::dataset::ParquetFileFormat> format = std::make_shared<arrow::dataset::ParquetFileFormat>();

        arrow::dataset::FileSystemFactoryOptions options;
        if (endsWithIgnoreCase(partOption.c_str(), "hivepartition"))
        {
            options.partitioning = arrow::dataset::HivePartitioning::MakeFactory();
        }
        else if (endsWithIgnoreCase(partOption.c_str(), "directorypartition"))
        {
            options.partitioning = arrow::dataset::DirectoryPartitioning::MakeFactory(partitionFields);
        }
        else
        {
            failx("Incorrect partitioning type %s.", partOption.c_str());
        }
        // Create the dataset factory
        PARQUET_ASSIGN_OR_THROW(auto datasetFactory, arrow::dataset::FileSystemDatasetFactory::Make(std::move(fs), std::move(selector), format, std::move(options)));

        // Get scanner
        PARQUET_ASSIGN_OR_THROW(auto dataset, datasetFactory->Finish());
        ARROW_ASSIGN_OR_RAISE(auto scanBuilder, dataset->NewScan());
        reportIfFailure(scanBuilder->Pool(pool));
        ARROW_ASSIGN_OR_RAISE(scanner, scanBuilder->Finish());
    }
    else
    {
        StringBuffer filename;
        StringBuffer path;
        splitFilename(location.c_str(), nullptr, &path, &filename, nullptr, false);
        Owned<IDirectoryIterator> itr = createDirectoryIterator(path.str(), filename.append("*.parquet"));

        auto readerProperties = parquet::ReaderProperties(pool);
        auto arrowReaderProps = parquet::ArrowReaderProperties();
        ForEach (*itr)
        {
            IFile &file = itr->query();
            parquet::arrow::FileReaderBuilder readerBuilder;
            reportIfFailure(readerBuilder.OpenFile(file.queryFilename(), false, readerProperties));
            readerBuilder.memory_pool(pool);
            readerBuilder.properties(arrowReaderProps);
            std::unique_ptr<parquet::arrow::FileReader> parquetFileReader;
            reportIfFailure(readerBuilder.Build(&parquetFileReader));
            parquetFileReaders.push_back(std::move(parquetFileReader));
        }
    }
    return arrow::Status::OK();
}

/**
 * @brief Divide row groups being read from a Parquet file among any number of thor workers.
 *
 * @param activityCtx Context information about which thor worker is reading the file.
 * @param totalRowGroups The total row groups in the file or files that are being read.
 * @param numRowGroups The number of row groups that this worker needs to read.
 * @param startRowGroup The starting row group index for each thor worker.
 */
void divide_row_groups(const IThorActivityContext *activityCtx, __int64 totalRowGroups, __int64 &numRowGroups, __int64 &startRowGroup)
{
    int workers = activityCtx->numSlaves();
    int strands = activityCtx->numStrands();
    int workerId = activityCtx->querySlave();

    // Currently under the assumption that all channels and workers are given a worker id and no matter
    // the configuration will show up in activityCtx->numSlaves()
    if (workers > 1)
    {
        // If the number of workers goes into totalRowGroups evenly then every worker gets the same amount
        // of rows to read
        if (totalRowGroups % workers == 0)
        {
            numRowGroups = totalRowGroups / workers;
            startRowGroup = numRowGroups * workerId;
        }
        // If the totalRowGroups is not evenly divisible by the number of workers then we divide them up
        // with the first n-1 workers getting slightly more and the nth worker gets the remainder
        else if (totalRowGroups > workers)
        {
            __int64 groupsPerWorker = totalRowGroups / workers;
            __int64 remainder = totalRowGroups % workers;

            if (workerId < remainder)
            {
                numRowGroups = groupsPerWorker + 1;
                startRowGroup = numRowGroups * workerId;
            }
            else
            {
                numRowGroups = groupsPerWorker;
                startRowGroup = (remainder * (numRowGroups + 1)) + ((workerId - remainder) * numRowGroups);
            }
        }
        // If the number of totalRowGroups is less than the number of workers we give as many as possible
        // a single row group to read.
        else
        {
            if (workerId < totalRowGroups)
            {
                numRowGroups = 1;
                startRowGroup = workerId;
            }
            else
            {
                numRowGroups = 0;
                startRowGroup = 0;
            }
        }
    }
    else
    {
        // There is only one worker
        numRowGroups = totalRowGroups;
        startRowGroup = 0;
    }
}

/**
 * @brief Splits an arrow table into an unordered map with the left side containing the
 * column names and the right side containing an Array of the column values.
 *
 * @param table The table to be split and stored in the unordered map.
*/
void ParquetReader::splitTable(std::shared_ptr<arrow::Table> &table)
{
    auto columns = table->columns();
    parquetTable.clear();
    for (int i = 0; i < columns.size(); i++)
    {
        parquetTable.insert(std::make_pair(table->field(i)->name(), columns[i]->chunk(0)));
    }
}

/**
 * @brief Get the current table taking into account multiple files with variable table counts.
 *
 * @param currTable The index of the current table relative to the total number in all files being read.
 * @return std::shared_ptr<parquet::arrow::RowGroupReader> The RowGroupReader to read columns from the table.
 */
std::shared_ptr<parquet::arrow::RowGroupReader> ParquetReader::queryCurrentTable(__int64 currTable)
{
    __int64 tables = 0;
    __int64 offset = 0;
    for (int i = 0; i < parquetFileReaders.size(); i++)
    {
        tables += fileTableCounts[i];
        if (currTable < tables)
        {
            return parquetFileReaders[i]->RowGroup(currTable - offset);
        }
        offset = tables;
    }
    failx("Failed getting RowGroupReader. Index %lli is out of bounds.", currTable);
    return nullptr;
}

/**
 * @brief Open the file reader for the target file and read the metadata for the row counts.
 *
 * @return arrow::Status Returns ok if opening a file and reading the metadata succeeds.
 */
arrow::Status ParquetReader::processReadFile()
{
    reportIfFailure(openReadFile()); // Open the file with the target location before processing.
    if (endsWithIgnoreCase(partOption.c_str(), "partition"))
    {
        PARQUET_ASSIGN_OR_THROW(rbatchReader, scanner->ToRecordBatchReader());
        rbatchItr = arrow::RecordBatchReader::RecordBatchReaderIterator(rbatchReader.get());
        PARQUET_ASSIGN_OR_THROW(auto datasetRows, scanner->CountRows());
        // Divide the work among any number of workers
        divide_row_groups(activityCtx, datasetRows, totalRowCount, startRowGroup);
    }
    else
    {
        __int64 totalTables = 0;

        for (int i = 0; i < parquetFileReaders.size(); i++)
        {
            __int64 tables = parquetFileReaders[i]->num_row_groups();
            fileTableCounts.push_back(tables);
            totalTables += tables;
        }

        divide_row_groups(activityCtx, totalTables, tableCount, startRowGroup);
    }
    tablesProcessed = 0;
    totalRowsProcessed = 0;
    rowsProcessed = 0;
    rowsCount = 0;
    return arrow::Status::OK();
}

/**
 * @brief Checks if all the rows have been read in a partitioned dataset, or if reading a single file checks if
 * all the RowGroups and every row in the last group has been read.
 *
 * @return True if there are more rows to be read and false if else.
 */
bool ParquetReader::shouldRead()
{
    if (scanner)
        return !(totalRowsProcessed >= totalRowCount);
    else
        return !(tablesProcessed >= tableCount && rowsProcessed >= rowsCount);
}

/**
 * @brief Iterates to the correct starting RecordBatch in a partitioned dataset.
 *
 * @return arrow::Result A pointer to the current table.
 */
arrow::Result<std::shared_ptr<arrow::Table>> ParquetReader::queryRows()
{
    // If no tables have been processed find the starting RecordBatch
    if (tablesProcessed == 0)
    {
        // Start by getting the number of rows in the first group and checking if it includes this workers startRow
        __int64 offset = (*rbatchItr)->get()->num_rows();
        while (offset < startRow)
        {
            rbatchItr++;
            offset += (*rbatchItr)->get()->num_rows();
        }
        // If startRow is in the middle of a table skip processing the beginning of the batch
        rowsProcessed = (*rbatchItr)->get()->num_rows() - (offset - startRow);
    }
    // Convert the current batch to a table
    PARQUET_ASSIGN_OR_THROW(auto batch, *rbatchItr);
    rbatchItr++;
    std::vector<std::shared_ptr<arrow::RecordBatch>> toTable = {std::move(batch)};
    return std::move(arrow::Table::FromRecordBatches(std::move(toTable)));
}

/**
 * @brief Updates the current table if all the rows have been proccessed. Sets nextTable to the current TableColumns object.
 *
 * @param nextTable The memory address of the TableColumns object containing the current table.
 * @return __int64 The number of rows that have been processed and the current index in the columns.
 */
__int64 ParquetReader::next(TableColumns *&nextTable)
{
    if (rowsProcessed == rowsCount || restoredCursor)
    {
        if (restoredCursor)
            restoredCursor = false;
        else
            rowsProcessed = 0;
        std::shared_ptr<arrow::Table> table;
        if (endsWithIgnoreCase(partOption.c_str(), "partition"))
        {
            PARQUET_ASSIGN_OR_THROW(table, queryRows()); // Sets rowsProcessed to current row in table corresponding to startRow
        }
        else
        {
            reportIfFailure(queryCurrentTable(tablesProcessed + startRowGroup)->ReadTable(&table));
        }
        tablesProcessed++;
        rowsCount = table->num_rows();
        splitTable(table);
    }
    nextTable = &parquetTable;
    totalRowsProcessed++;
    return rowsProcessed++;
}

/**
 * @brief Gets the information about the next row to be read from a Parquet file. A boolean is stored at the
 * beginning of the memory buffer for the partition status. If the file is a partitioned dataset, the next row
 * and the number of remaining rows are stored in the memory buffer. If the file is a regular Parquet file, the
 * current and remaining tables are stored in the memory buffer. Additionally, the number of rows processed within
 * the current table is stored in the memory buffer.
 *
 * @param cursor MemoryBuffer where file processing information is stored
 * @return true If building the buffer succeeds
 */
bool ParquetReader::getCursor(MemoryBuffer & cursor)
{
    bool partition = endsWithIgnoreCase(partOption.c_str(), "partition");
    cursor.append(partition);

    // Adjust starting positions to current read position and remove
    // already processed rows from the total count for the workers
    if (partition)
    {
        cursor.append(startRow + totalRowsProcessed);
        cursor.append(totalRowCount - totalRowsProcessed);
    }
    else
    {
        cursor.append(startRowGroup + tablesProcessed);
        cursor.append(tableCount - tablesProcessed);
        cursor.append(rowsProcessed);
    }

    return true;
}

/**
 * @brief Resets the current access row in the Parquet file based on the information stored in a memory buffer
 * created by getCursor. Sets restoredCursor to true for reading from the middle of the table in non-partitioned
 * datasets. Resets the file read process trackers and reads the partition flag from the beginning of the buffer.
 * If the file is a partitioned dataset then only the starting and remaining rows are read. Otherwise the
 * starting and remaining tables are read as well as the current row within the table.
 *
 * @param cursor MemoryBuffer where file processing information is stored
 */
void ParquetReader::setCursor(MemoryBuffer & cursor)
{
    restoredCursor = true;
    tablesProcessed = 0;
    totalRowsProcessed = 0;
    rowsProcessed = 0;
    rowsCount = 0;

    bool partition;
    cursor.read(partition);
    if (partition)
    {
        cursor.read(startRow);
        cursor.read(totalRowCount);
    }
    else
    {
        cursor.read(startRowGroup);
        cursor.read(tableCount);
        cursor.read(rowsProcessed);
    }
}

/**
 * @brief Constructs a ParquetWriter for the target destination and checks for existing data.
 *
 * @param option The read or write option as well as information about partitioning.
 * @param _destination The full path to which to write a Parquet file or partitioned dataset. Can be a filename or directory.
 * @param _maxRowCountInBatch The max number of rows when creating RecordBatches for output.
 * @param _overwrite If true when the plugin calls checkDirContents the target directory contents will be deleted.
 * @param _compressionOption Compression option for writing compressed Parquet files of different types.
 * @param _activityCtx Additional context about the thor workers running.
 */
ParquetWriter::ParquetWriter(const char *option, const char *_destination, int _maxRowCountInBatch, bool _overwrite, arrow::Compression::type _compressionOption, const char *_partitionFields, const IThorActivityContext *_activityCtx)
    : partOption(option), destination(_destination), maxRowCountInBatch(_maxRowCountInBatch), overwrite(_overwrite), compressionOption(_compressionOption), activityCtx(_activityCtx)
{
    pool = arrow::default_memory_pool();
    parquetDoc = std::vector<rapidjson::Document>(maxRowCountInBatch);
    if (activityCtx->querySlave() == 0 && startsWithIgnoreCase(partOption.c_str(), "write"))
    {
        reportIfFailure(checkDirContents());
    }
    if (endsWithIgnoreCase(partOption.c_str(), "partition"))
    {
        std::stringstream ss(_partitionFields);
        std::string field;
        while (std::getline(ss, field, ';'))
        {
            partitionFields.push_back(field);
        }
    }
}

ParquetWriter::~ParquetWriter()
{
    pool->ReleaseUnused();
    jsonAlloc.Clear();
}

/**
 * @brief Opens a write stream depending on if the user is writing a partitioned file or regular file.
 *
 * @return Status object arrow::Status::OK if successful.
 */
arrow::Status ParquetWriter::openWriteFile()
{
    if (destination.empty())
    {
        failx("Invalid option: The destination was not supplied.");
    }
    if (endsWithIgnoreCase(partOption.c_str(), "partition"))
    {
        ARROW_ASSIGN_OR_RAISE(auto filesystem, arrow::fs::FileSystemFromUriOrPath(destination));
        auto format = std::make_shared<arrow::dataset::ParquetFileFormat>();
        writeOptions.file_write_options = format->DefaultWriteOptions();
        writeOptions.filesystem = std::move(filesystem);
        writeOptions.base_dir = destination;
        writeOptions.partitioning = partitionType;
        writeOptions.existing_data_behavior = arrow::dataset::ExistingDataBehavior::kOverwriteOrIgnore;
    }
    else
    {
        if(!endsWith(destination.c_str(), ".parquet"))
            failx("Error opening file: Invalid file extension for file %s", destination.c_str());

        // Currently under the assumption that all channels and workers are given a worker id and no matter
        // the configuration will show up in activityCtx->numSlaves()
        if (activityCtx->numSlaves() > 1)
        {
            destination.insert(destination.find(".parquet"), std::to_string(activityCtx->querySlave()));
        }

        std::shared_ptr<arrow::io::FileOutputStream> outfile;
        PARQUET_ASSIGN_OR_THROW(outfile, arrow::io::FileOutputStream::Open(destination));

        // Choose compression
        std::shared_ptr<parquet::WriterProperties> props = parquet::WriterProperties::Builder().compression(compressionOption)->build();

        // Opt to store Arrow schema for easier reads back into Arrow
        std::shared_ptr<parquet::ArrowWriterProperties> arrowProps = parquet::ArrowWriterProperties::Builder().store_schema()->build();

        // Create a writer
        ARROW_ASSIGN_OR_RAISE(writer, parquet::arrow::FileWriter::Open(*schema.get(), pool, outfile, std::move(props), std::move(arrowProps)));
    }
    return arrow::Status::OK();
}

/**
 * @brief Writes a single record batch to a partitioned dataset.
 *
 * @param table An arrow table to write out.
 * @return Status object arrow::Status::OK if successful.
*/
arrow::Status ParquetWriter::writePartition(std::shared_ptr<arrow::Table> table)
{
    // Create dataset for writing partitioned files.
    auto dataset = std::make_shared<arrow::dataset::InMemoryDataset>(table);

    StringBuffer basenameTemplate;
    basenameTemplate.appendf("part_%d{i}_%lld.parquet",activityCtx->querySlave(), tablesProcessed++);
    writeOptions.basename_template = basenameTemplate.str();

    ARROW_ASSIGN_OR_RAISE(auto scannerBuilder, dataset->NewScan());
    reportIfFailure(scannerBuilder->Pool(pool));
    ARROW_ASSIGN_OR_RAISE(auto scanner, scannerBuilder->Finish());

    // Write partitioned files.
    reportIfFailure(arrow::dataset::FileSystemDataset::Write(writeOptions, scanner));

    return arrow::Status::OK();
}

/**
 * @brief Converts the vector of rapidjson::Documents into an arrow::RecordBatch and writes it to
 * a file or partitioned dataset.
 */
void ParquetWriter::writeRecordBatch()
{
    // Convert row_batch vector to RecordBatch and write to file.
    PARQUET_ASSIGN_OR_THROW(auto recordBatch, convertToRecordBatch(parquetDoc, schema));
    // Write each batch as a row_groups
    PARQUET_ASSIGN_OR_THROW(auto table, arrow::Table::FromRecordBatches(schema, {recordBatch}));

    if (endsWithIgnoreCase(partOption.c_str(), "partition"))
    {
        reportIfFailure(writePartition(table));
    }
    else
    {
        reportIfFailure(writer->WriteTable(*(table.get()), recordBatch->num_rows()));
    }
}

/**
 * @brief Converts the vector of rapidjson::Documents into an arrow::RecordBatch and writes it to
 * a file or partitioned dataset. Resizes the vector before converting to a RecordBatch.
 *
 * @param newSize The new size of the vector.
 */
void ParquetWriter::writeRecordBatch(std::size_t newSize)
{
    parquetDoc.resize(newSize);
    writeRecordBatch();
}

/**
 * @brief Returns a pointer to the top of the stack for the current row being built.
 *
 * @return A pointer to the rapidjson::Value containing the row
 */
rapidjson::Value *ParquetWriter::queryCurrentRow()
{
    return &rowStack[rowStack.size() - 1];
}

/**
 * @brief A helper method for updating the current row on writes and keeping
 * it within the boundary of the maxRowCountInBatch set by the user when creating RowGroups.
 */
void ParquetWriter::updateRow()
{
    if (++currentRow == maxRowCountInBatch)
        currentRow = 0;
}

/**
 * @brief Convert a vector of rapidjson::Documents containing single rows to an arrow::RecordBatch
 *
 * @param rows The vector of rows to be converted.
 * @param schema The arrow::Schema of the rows being converted.
 * @return An arrow::Result object containing the new RecordBatch.
 */
arrow::Result<std::shared_ptr<arrow::RecordBatch>> ParquetWriter::convertToRecordBatch(const std::vector<rapidjson::Document> &rows, std::shared_ptr<arrow::Schema> schema)
{
    // Create RecordBatchBuilder from schema and set the size
    std::unique_ptr<arrow::RecordBatchBuilder> batchBuilder;
    ARROW_ASSIGN_OR_RAISE(batchBuilder, arrow::RecordBatchBuilder::Make(schema, pool, rows.size()));

    JsonValueConverter converter(rows);
    for (int i = 0; i < batchBuilder->num_fields(); ++i)
    {
        std::shared_ptr<arrow::Field> field = schema->field(i);
        arrow::ArrayBuilder *builder = batchBuilder->GetField(i);
        ARROW_RETURN_NOT_OK(converter.Convert(*field.get(), builder));
    }

    std::shared_ptr<arrow::RecordBatch> batch;
    ARROW_ASSIGN_OR_RAISE(batch, batchBuilder->Flush());

    // Use RecordBatch::ValidateFull() to make sure arrays were correctly constructed.
    reportIfFailure(batch->ValidateFull());
    return batch;
}

/**
 * @brief Creates the child record for an array or dataset type. This method is used for converting
 * the ECL RtlFieldInfo object into arrow::Fields for creating a rapidjson document object.
 *
 * @param field The field containing metadata for the record.
 * @returns An arrow::NestedType holding the schema and fields of the child records.
 */
std::shared_ptr<arrow::NestedType> ParquetWriter::makeChildRecord(const RtlFieldInfo *field)
{
    const RtlTypeInfo *typeInfo = field->type;
    const RtlFieldInfo *const *fields = typeInfo->queryFields();
    // Create child fields
    if (fields)
    {
        int count = getNumFields(typeInfo);

        std::vector<std::shared_ptr<arrow::Field>> childFields;

        for (int i = 0; i < count; i++, fields++)
        {
            reportIfFailure(fieldToNode((*fields)->name, *fields, childFields));
        }

        return std::make_shared<arrow::StructType>(childFields);
    }
    else
    {
        // Create set
        const RtlTypeInfo *child = typeInfo->queryChildType();
        const RtlFieldInfo childFieldInfo = RtlFieldInfo("", "", child);
        std::vector<std::shared_ptr<arrow::Field>> childField;
        reportIfFailure(fieldToNode(childFieldInfo.name, &childFieldInfo, childField));
        return std::make_shared<arrow::ListType>(childField[0]);
    }
}

/**
 * @brief Converts an RtlFieldInfo object into an arrow field and adds it to the output vector.
 *
 * @param name The name of the field
 * @param field The field containing metadata for the record.
 * @param arrowFields Output vector for pushing new nodes to.
 * @return Status of the operation
 */
arrow::Status ParquetWriter::fieldToNode(const std::string &name, const RtlFieldInfo *field, std::vector<std::shared_ptr<arrow::Field>> &arrowFields)
{
    unsigned len = field->type->length;

    switch (field->type->getType())
    {
    case type_boolean:
        arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::boolean()));
        break;
    case type_int:
        if (field->type->isSigned())
        {
            if (len > 4)
            {
                arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::int64()));
            }
            else
            {
                arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::int32()));
            }
        }
        else
        {
            if (len > 4)
            {
                arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::uint64()));
            }
            else
            {
                arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::uint32()));
            }
        }
        break;
    case type_real:
        arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::float64()));
        break;
    case type_string:
        arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::utf8()));
        break;
    case type_char:
        arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::utf8()));
        break;
    case type_varstring:
        arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::utf8()));
        break;
    case type_qstring:
        arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::utf8()));
        break;
    case type_unicode:
        arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::utf8()));
        break;
    case type_utf8:
        arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::utf8()));
        break;
    case type_decimal:
        arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::utf8()));
        break;
    case type_data:
        arrowFields.push_back(std::make_shared<arrow::Field>(name, arrow::large_binary()));
        break;
    case type_record:
        arrowFields.push_back(std::make_shared<arrow::Field>(name, makeChildRecord(field)));
        break;
    case type_set:
        arrowFields.push_back(std::make_shared<arrow::Field>(name, makeChildRecord(field)));
        break;
    default:
        failx("Datatype %i is not compatible with this plugin.", field->type->getType());
    }

    return arrow::Status::OK();
}

/**
 * @brief Creates an arrow::Schema from the field info of the row.
 *
 * @param typeInfo An RtlTypeInfo object that we iterate through to get all
 * the information for the row.
 */
arrow::Status ParquetWriter::fieldsToSchema(const RtlTypeInfo *typeInfo)
{
    const RtlFieldInfo *const *fields = typeInfo->queryFields();
    int count = getNumFields(typeInfo);

    std::vector<std::shared_ptr<arrow::Field>> arrowFields;

    for (int i = 0; i < count; i++, fields++)
    {
        ARROW_RETURN_NOT_OK(fieldToNode((*fields)->name, *fields, arrowFields));
    }

    schema = std::make_shared<arrow::Schema>(arrowFields);

    // If writing a partitioned file also create the partitioning schema from the partitionFields set by the user
    if (endsWithIgnoreCase(partOption.c_str(), "partition"))
    {
        arrow::FieldVector partitionSchema;
        for (int i = 0; i < partitionFields.size(); i++)
        {
            auto field = schema->GetFieldByName(partitionFields[i]);
            if (field)
                partitionSchema.push_back(field);
            else
                failx("Field %s not found in RECORD definition of Parquet file.", partitionFields[i].c_str());
        }

        if (endsWithIgnoreCase(partOption.c_str(), "hivepartition"))
            partitionType = std::make_shared<arrow::dataset::HivePartitioning>(std::make_shared<arrow::Schema>(partitionSchema));
        else if (endsWithIgnoreCase(partOption.c_str(), "directorypartition"))
            partitionType = std::make_shared<arrow::dataset::DirectoryPartitioning>(std::make_shared<arrow::Schema>(partitionSchema));
        else
            failx("Partitioning method %s is not supported.", partOption.c_str());
    }

    return arrow::Status::OK();
}

/**
 * @brief Creates a rapidjson::Value with an array type and adds it to the stack
 */
void ParquetWriter::beginSet()
{
    rapidjson::Value row(rapidjson::kArrayType);
    rowStack.push_back(std::move(row));
}

/**
 * @brief Creates a rapidjson::Value with an object type and adds it to the stack
 */
void ParquetWriter::beginRow()
{
    rapidjson::Value row(rapidjson::kObjectType);
    rowStack.push_back(std::move(row));
}

/**
 * @brief Removes the value from the top of the stack and adds it the parent row.
 * If there is only one value on the stack then it converts it to a rapidjson::Document.
 *
 * @param name The name of the row field.
 */
void ParquetWriter::endRow(const char *name)
{
    if (rowStack.size() > 1)
    {
        rapidjson::Value child = std::move(rowStack[rowStack.size() - 1]);
        rowStack.pop_back();
        rowStack[rowStack.size() - 1].AddMember(rapidjson::StringRef(name), child, jsonAlloc);
    }
    else
    {
        parquetDoc[currentRow].SetObject();

        rapidjson::Value parent = std::move(rowStack[rowStack.size() - 1]);
        rowStack.pop_back();

        for (auto itr = parent.MemberBegin(); itr != parent.MemberEnd(); ++itr)
        {
            parquetDoc[currentRow].AddMember(itr->name, itr->value, jsonAlloc);
        }
    }
}

/**
 * @brief Adds a key value pair to the current row being built for writing to parquet.
 *
 * @param key Field name of the column.
 * @param value Value of the field.
 */
void ParquetWriter::addMember(rapidjson::Value &key, rapidjson::Value &value)
{
    rapidjson::Value *row = &rowStack[rowStack.size() - 1];
    if(!row)
        failx("Failed to add member to rapidjson row");
    if (row->GetType() == rapidjson::kObjectType)
        row->AddMember(key, value, jsonAlloc);
    else
        row->PushBack(value, jsonAlloc);
}

/**
 * @brief Check the contents of the target location set by the user. If the overwrite option
 * is true then any files in the target directory or matching the file mask will be deleted.
 *
 * @return arrow::Status::OK if all operations successful.
 */
arrow::Status ParquetWriter::checkDirContents()
{
    if (destination.empty())
    {
        failx("Missing target location when writing Parquet data.");
    }
    StringBuffer path;
    StringBuffer filename;
    StringBuffer ext;
    splitFilename(destination.c_str(), nullptr, &path, &filename, &ext, false);

    ARROW_ASSIGN_OR_RAISE(auto filesystem, arrow::fs::FileSystemFromUriOrPath(destination));

    Owned<IDirectoryIterator> itr = createDirectoryIterator(path.str(), filename.appendf("*%s", ext.str()));
    ForEach (*itr)
    {
        IFile &file = itr->query();
        if (file.isFile() == fileBool::foundYes)
        {
            if(overwrite)
            {
                if (!file.remove())
                {
                    failx("Failed to remove file %s", file.queryFilename());
                }
            }
            else
            {
                failx("The target file %s already exists. To delete the file set the overwrite option to true.", file.queryFilename());
            }
        }
        else
        {
            if (overwrite)
            {
                reportIfFailure(filesystem->DeleteDirContents(path.str()));
                break;
            }
            else
            {
                failx("The target directory %s is not empty. To delete the contents of the directory set the overwrite option to true.", path.str());
            }
        }
    }
    return arrow::Status::OK();
}

/**
 * @brief Create a ParquetRowBuilder and build a row. If all the rows in a table have been processed a
 * new table will be read from the input file.
 *
 * @return const void * Memory Address where result row is stored.
 */
const void *ParquetRowStream::nextRow()
{
    if (shouldRead && parquetReader->shouldRead())
    {
        TableColumns *table = nullptr;
        auto index = parquetReader->next(table);
        currentRow++;

        if (table && !table->empty())
        {
            ParquetRowBuilder pRowBuilder(table, index);

            RtlDynamicRowBuilder rowBuilder(resultAllocator);
            const RtlTypeInfo *typeInfo = resultAllocator->queryOutputMeta()->queryTypeInfo();
            assertex(typeInfo);
            RtlFieldStrInfo dummyField("<row>", NULL, typeInfo);
            size32_t len = typeInfo->build(rowBuilder, 0, &dummyField, pRowBuilder);
            return rowBuilder.finalizeRowClear(len);
        }
        else
            failx("Error processing result row");
    }
    return nullptr;
}

/**
 * @brief Stop reading result rows from the Parquet file.
 */
void ParquetRowStream::stop()
{
    resultAllocator.clear();
    shouldRead = false;
}

/**
 * @brief Utility function for getting the xpath or field name from an RtlFieldInfo object.
 *
 * @param outXPath The buffer for storing output.
 * @param field RtlFieldInfo object storing metadata for field.
 */
void ParquetRowBuilder::xpathOrName(StringBuffer &outXPath, const RtlFieldInfo *field) const
{
    outXPath.clear();

    if (field->xpath)
    {
        if (field->xpath[0] == xpathCompoundSeparatorChar)
        {
            outXPath.append(field->xpath + 1);
        }
        else
        {
            const char *sep = strchr(field->xpath, xpathCompoundSeparatorChar);

            if (!sep)
            {
                outXPath.append(field->xpath);
            }
            else
            {
                outXPath.append(field->xpath, 0, static_cast<size32_t>(sep - field->xpath));
            }
        }
    }
    else
    {
        outXPath.append(field->name);
    }
}

/**
 * @brief Gets the current array index taking into account the nested status of the row.
 *
 * @return int64_t The current array index of the value.
 */
int64_t ParquetRowBuilder::currArrayIndex()
{
    return !pathStack.empty() && pathStack.back().nodeType == CPNTSet ? pathStack.back().childrenProcessed++ : currentRow;
}

/**
 * @brief Returns a Signed value depending on the size of the integer that was stored in parquet.
 *
 * @param arrayVisitor The ParquetVisitor class for getting a pointer to the column.
 * @param index The index in the array to read a value from.
 * @return __int64 Result value in the array..
 */
__int64 getSigned(std::shared_ptr<ParquetArrayVisitor> &arrayVisitor, int index)
{
    switch (arrayVisitor->size)
    {
        case 8:
            return arrayVisitor->int8Arr->Value(index);
        case 16:
            return arrayVisitor->int16Arr->Value(index);
        case 32:
            return arrayVisitor->int32Arr->Value(index);
        case 64:
            return arrayVisitor->int64Arr->Value(index);
        default:
            failx("getSigned: Invalid size %i", arrayVisitor->size);
    }
    return 0;
}

/**
 * @brief Returns an Unsigned value depending on the size of the unsigned integer that was stored in parquet.
 *
 * @param arrayVisitor The ParquetVisitor class for getting a pointer to the column.
 * @param index The index in the array to read a value from.
 * @return unsigned __int64 Result value in the array.
 */
unsigned __int64 getUnsigned(std::shared_ptr<ParquetArrayVisitor> &arrayVisitor, int index)
{
    switch (arrayVisitor->size)
    {
        case 8:
            return arrayVisitor->uint8Arr->Value(index);
        case 16:
            return arrayVisitor->uint16Arr->Value(index);
        case 32:
            return arrayVisitor->uint32Arr->Value(index);
        case 64:
            return arrayVisitor->uint64Arr->Value(index);
        default:
            failx("getUnsigned: Invalid size %i", arrayVisitor->size);
    }
    return 0;
}

/**
 * @brief Returns a Real value depending on the size of the double that was stored in parquet.
 *
 * @param arrayVisitor The ParquetVisitor class for getting a pointer to the column.
 * @param index The index in the array to read a value from.
 * @return double Result value in the array.
 */
double getReal(std::shared_ptr<ParquetArrayVisitor> &arrayVisitor, int index)
{
    switch (arrayVisitor->size)
    {
        case 2:
            return arrayVisitor->halfFloatArr->Value(index);
        case 4:
            return arrayVisitor->floatArr->Value(index);
        case 8:
            return arrayVisitor->doubleArr->Value(index);
        default:
            failx("getReal: Invalid size %i", arrayVisitor->size);
    }
    return 0;
}

/**
 * @brief Gets the value as a string_view. If the field is a numeric type it is serialized to a StringBuffer.
 *
 * @param field Field information used for warning the user if a type is unsupported.
 * @return std::string_view A view of the current result.
 */
std::string_view ParquetRowBuilder::getCurrView(const RtlFieldInfo *field)
{
    serialized.clear();

    switch(arrayVisitor->type)
    {
        case BoolType:
            serialized.append(arrayVisitor->boolArr->Value(currArrayIndex()));
            return serialized.str();
        case BinaryType:
            return arrayVisitor->binArr->GetView(currArrayIndex());
        case LargeBinaryType:
            return arrayVisitor->largeBinArr->GetView(currArrayIndex());
        case RealType:
            serialized.append(getReal(arrayVisitor, currArrayIndex()));
            return serialized.str();
        case IntType:
            serialized.append(getSigned(arrayVisitor, currArrayIndex()));
            return serialized.str();
        case UIntType:
            serialized.append(getUnsigned(arrayVisitor, currArrayIndex()));
            return serialized.str();
        case DateType:
            serialized.append(arrayVisitor->size == 32 ? (__int32) arrayVisitor->date32Arr->Value(currArrayIndex()) : (__int64) arrayVisitor->date64Arr->Value(currArrayIndex()));
            return serialized.str();
        case TimestampType:
            serialized.append((__int64) arrayVisitor->timestampArr->Value(currArrayIndex()));
            return serialized.str();
        case TimeType:
            serialized.append(arrayVisitor->size == 32 ? (__int32) arrayVisitor->time32Arr->Value(currArrayIndex()) : (__int64) arrayVisitor->time64Arr->Value(currArrayIndex()));
            return serialized.str();
        case DurationType:
            serialized.append((__int64) arrayVisitor->durationArr->Value(currArrayIndex()));
            return serialized.str();
        case StringType:
            return arrayVisitor->stringArr->GetView(currArrayIndex());
        case LargeStringType:
            return arrayVisitor->largeStringArr->GetView(currArrayIndex());
        case DecimalType:
            return arrayVisitor->size == 128 ? arrayVisitor->decArr->GetView(currArrayIndex()) : arrayVisitor->largeDecArr->GetView(currArrayIndex());
        default:
            failx("Unimplemented Parquet type for field with name %s.", field->name);
    }
    return "";
}

/**
 * @brief Get the current value as an Integer.
 *
 * @param field Field information used for warning the user if a type is unsupported.
 * @return __int64 The current value in the column.
 */
__int64 ParquetRowBuilder::getCurrIntValue(const RtlFieldInfo *field)
{
    switch (arrayVisitor->type)
    {
        case BoolType:
            return arrayVisitor->boolArr->Value(currArrayIndex());
        case IntType:
            return getSigned(arrayVisitor, currArrayIndex());
        case UIntType:
            return getUnsigned(arrayVisitor, currArrayIndex());
        case RealType:
            return getReal(arrayVisitor, currArrayIndex());
        case DateType:
            return arrayVisitor->size == 32 ? arrayVisitor->date32Arr->Value(currArrayIndex()) : arrayVisitor->date64Arr->Value(currArrayIndex());
        case TimestampType:
            return arrayVisitor->timestampArr->Value(currArrayIndex());
        case TimeType:
            return arrayVisitor->size == 32 ? arrayVisitor->time32Arr->Value(currArrayIndex()) : arrayVisitor->time64Arr->Value(currArrayIndex());
        case DurationType:
            return arrayVisitor->durationArr->Value(currArrayIndex());
        default:
        {
            auto scalar = getCurrView(field);
            return rtlStrToInt8(scalar.size(), scalar.data());
        }
    }
}

/**
 * @brief Get the current value as a Double.
 *
 * @param field Field information used for warning the user if a type is unsupported.
 * @return double The current value in the column.
 */
double ParquetRowBuilder::getCurrRealValue(const RtlFieldInfo *field)
{
    switch (arrayVisitor->type)
    {
        case BoolType:
            return arrayVisitor->boolArr->Value(currArrayIndex());
        case IntType:
            return getSigned(arrayVisitor, currArrayIndex());
        case UIntType:
            return getUnsigned(arrayVisitor, currArrayIndex());
        case RealType:
            return getReal(arrayVisitor, currArrayIndex());
        case DateType:
            return arrayVisitor->size == 32 ? arrayVisitor->date32Arr->Value(currArrayIndex()) : arrayVisitor->date64Arr->Value(currArrayIndex());
        case TimestampType:
            return arrayVisitor->timestampArr->Value(currArrayIndex());
        case TimeType:
            return arrayVisitor->size == 32 ? arrayVisitor->time32Arr->Value(currArrayIndex()) : arrayVisitor->time64Arr->Value(currArrayIndex());
        case DurationType:
            return arrayVisitor->durationArr->Value(currArrayIndex());
        default:
        {
            auto scalar = getCurrView(field);
            return rtlStrToReal(scalar.size(), scalar.data());
        }
    }
}

/**
 * @brief Gets a Boolean result for an ECL Row
 *
 * @param field Holds the value of the field.
 * @return bool Returns the boolean value from the result row.
 */
bool ParquetRowBuilder::getBooleanResult(const RtlFieldInfo *field)
{
    nextField(field);

    if (arrayVisitor->type == NullType)
    {
        NullFieldProcessor p(field);
        return p.boolResult;
    }

    return getCurrIntValue(field);
}

/**
 * @brief Gets a Data value from the result row.
 *
 * @param field Holds the value of the field.
 * @param len Length of the Data value.
 * @param result Pointer to return value stored in memory.
 */
void ParquetRowBuilder::getDataResult(const RtlFieldInfo *field, size32_t &len, void *&result)
{
    nextField(field);

    if (arrayVisitor->type == NullType)
    {
        NullFieldProcessor p(field);
        rtlUtf8ToDataX(len, result, p.resultChars, p.stringResult);
        return;
    }

    auto view = getCurrView(field);
    rtlStrToDataX(len, result, view.size(), view.data());
    return;
}

/**
 * @brief Gets a Real value from the result row.
 *
 * @param field Holds the value of the field.
 * @return double Double value to return.
 */
double ParquetRowBuilder::getRealResult(const RtlFieldInfo *field)
{
    nextField(field);

    if (arrayVisitor->type == NullType)
    {
        NullFieldProcessor p(field);
        return p.doubleResult;
    }

    return getCurrRealValue(field);
}

/**
 * @brief Gets the Signed Integer value from the result row.
 *
 * @param field Holds the value of the field.
 * @return __int64 Value to return.
 */
__int64 ParquetRowBuilder::getSignedResult(const RtlFieldInfo *field)
{
    nextField(field);

    if (arrayVisitor->type == NullType)
    {
        NullFieldProcessor p(field);
        return p.intResult;
    }

    return getCurrIntValue(field);
}

/**
 * @brief Gets the Unsigned Integer value from the result row.
 *
 * @param field Holds the value of the field.
 * @return unsigned Value to return.
 */
unsigned __int64 ParquetRowBuilder::getUnsignedResult(const RtlFieldInfo *field)
{
    nextField(field);

    if (arrayVisitor->type == NullType)
    {
        NullFieldProcessor p(field);
        return p.uintResult;
    }

    if (arrayVisitor->type == UIntType)
        return getUnsigned(arrayVisitor, currArrayIndex());
    else
        return getCurrIntValue(field);
}

/**
 * @brief Gets a String from the result row.
 *
 * @param field Holds the value of the field.
 * @param chars Number of chars in the String.
 * @param result Pointer to return value stored in memory.
 */
void ParquetRowBuilder::getStringResult(const RtlFieldInfo *field, size32_t &chars, char *&result)
{
    nextField(field);

    if (arrayVisitor->type == NullType)
    {
        NullFieldProcessor p(field);
        rtlUtf8ToStrX(chars, result, p.resultChars, p.stringResult);
        return;
    }
    auto view = getCurrView(field);
    unsigned numchars = rtlUtf8Length(view.size(), view.data());
    rtlUtf8ToStrX(chars, result, numchars, view.data());
    return;
}

/**
 * @brief Gets a UTF8 string from the result row.
 *
 * @param field Holds the value of the field.
 * @param chars Number of chars in the UTF8.
 * @param result Pointer to return value stored in memory.
 */
void ParquetRowBuilder::getUTF8Result(const RtlFieldInfo *field, size32_t &chars, char *&result)
{
    nextField(field);

    if (arrayVisitor->type == NullType)
    {
        NullFieldProcessor p(field);
        rtlUtf8ToUtf8X(chars, result, p.resultChars, p.stringResult);
        return;
    }
    auto view = getCurrView(field);
    unsigned numchars = rtlUtf8Length(view.size(), view.data());
    rtlUtf8ToUtf8X(chars, result, numchars, view.data());
    return;
}

/**
 * @brief Gets a Unicode string from the result row.
 *
 * @param field Holds the value of the field.
 * @param chars Number of chars in the Unicode.
 * @param result Pointer to return value stored in memory.
 */
void ParquetRowBuilder::getUnicodeResult(const RtlFieldInfo *field, size32_t &chars, UChar *&result)
{
    nextField(field);

    if (arrayVisitor->type == NullType)
    {
        NullFieldProcessor p(field);
        rtlUnicodeToUnicodeX(chars, result, p.resultChars, p.unicodeResult);
        return;
    }
    auto view = getCurrView(field);
    unsigned numchars = rtlUtf8Length(view.size(), view.data());
    rtlUtf8ToUnicodeX(chars, result, numchars, view.data());
    return;
}

/**
 * @brief Gets a decimal from the result row.
 *
 * @param field Holds the value of the field.
 * @param value Variable used for returning decimal to caller.
 */
void ParquetRowBuilder::getDecimalResult(const RtlFieldInfo *field, Decimal &value)
{
    nextField(field);

    if (arrayVisitor->type == NullType)
    {
        NullFieldProcessor p(field);
        value.set(p.decimalResult);
        return;
    }
    auto dvalue = getCurrView(field);
    value.setString(dvalue.size(), dvalue.data());
    RtlDecimalTypeInfo *dtype = (RtlDecimalTypeInfo *)field->type;
    value.setPrecision(dtype->getDecimalDigits(), dtype->getDecimalPrecision());
    return;
}

/**
 * @brief Starts a new Set.
 *
 * @param field Field with information about the context of the set.
 * @param isAll Not Supported.
 */
void ParquetRowBuilder::processBeginSet(const RtlFieldInfo *field, bool &isAll)
{
    isAll = false; // ALL not supported
    nextField(field);

    if (arrayVisitor->type == ListType)
    {
        PathTracker newPathNode(field->name, arrayVisitor->listArr, CPNTSet);
        newPathNode.childCount = arrayVisitor->listArr->value_slice(currentRow)->length();
        pathStack.push_back(newPathNode);
    }
    else
    {
        failx("Error reading nested set with name %s.", field->name);
    }
}

/**
 * @brief Checks if we should process another set.
 *
 * @param field Context information about the set.
 * @return true If the children that we have processed is less than the total child count.
 * @return false If all the children sets have been processed.
 */
bool ParquetRowBuilder::processNextSet(const RtlFieldInfo *field)
{
    return pathStack.back().finishedChildren();
}

/**
 * @brief Starts a new Dataset.
 *
 * @param field Information about the context of the dataset.
 */
void ParquetRowBuilder::processBeginDataset(const RtlFieldInfo *field)
{
    UNSUPPORTED("Nested Dataset type is unsupported.");
}

/**
 * @brief Starts a new Row.
 *
 * @param field Information about the context of the row.
 */
void ParquetRowBuilder::processBeginRow(const RtlFieldInfo *field)
{
    StringBuffer xpath;
    xpathOrName(xpath, field);

    if (!xpath.isEmpty())
    {
        if (strncmp(xpath, "<row>", 5) != 0)
        {
            nextField(field);
            if (arrayVisitor->type == StructType)
            {
                pathStack.push_back(PathTracker(field->name, arrayVisitor->structArr, CPNTScalar));
            }
            else
            {
                failx("proccessBeginRow: Incorrect type for row.");
            }
        }
    }
    else
    {
        failx("processBeginRow: Field name or xpath missing");
    }
}

/**
 * @brief Checks whether we should process the next row.
 *
 * @param field Information about the context of the row.
 * @return true If the number of child rows process is less than the total count of children.
 * @return false If all of the child rows have been processed.
 */
bool ParquetRowBuilder::processNextRow(const RtlFieldInfo *field)
{
    return pathStack.back().childrenProcessed < pathStack.back().childCount;
}

/**
 * @brief Ends a set.
 *
 * @param field Information about the context of the set.
 */
void ParquetRowBuilder::processEndSet(const RtlFieldInfo *field)
{
    StringBuffer xpath;
    xpathOrName(xpath, field);

    if (!xpath.isEmpty() && !pathStack.empty() && strcmp(xpath.str(), pathStack.back().nodeName) == 0)
    {
        pathStack.pop_back();
    }
}

/**
 * @brief Ends a dataset.
 *
 * @param field Information about the context of the dataset.
 */
void ParquetRowBuilder::processEndDataset(const RtlFieldInfo *field)
{
    UNSUPPORTED("Nested Dataset type is unsupported.");
}

/**
 * @brief Ends a row.
 *
 * @param field Information about the context of the row.
 */
void ParquetRowBuilder::processEndRow(const RtlFieldInfo *field)
{
    StringBuffer xpath;
    xpathOrName(xpath, field);

    if (!xpath.isEmpty())
    {
        if (!pathStack.empty())
        {
            if (pathStack.back().nodeType == CPNTDataset)
            {
                pathStack.back().childrenProcessed++;
            }
            else if (strcmp(xpath.str(), pathStack.back().nodeName) == 0)
            {
                pathStack.pop_back();
            }
        }
    }
    else
    {
        failx("processEndRow: Field name or xpath missing");
    }
}

/**
 * @brief Applies a visitor to the nested value of a Struct or List field.
 *
 * @param field Information about the context of the field.
 */
void ParquetRowBuilder::nextFromStruct(const RtlFieldInfo *field)
{
    auto structPtr = pathStack.back().structPtr;
    reportIfFailure(structPtr->Accept(arrayVisitor.get()));
    if (pathStack.back().nodeType == CPNTScalar)
    {
        auto child = arrayVisitor->structArr->GetFieldByName(field->name);
        reportIfFailure(child->Accept(arrayVisitor.get()));
    }
    else if (pathStack.back().nodeType == CPNTSet)
    {
        auto child = arrayVisitor->listArr->value_slice(currentRow);
        reportIfFailure(child->Accept(arrayVisitor.get()));
    }
}

/**
 * @brief Gets the next field and processes it.
 *
 * @param field Information about the context of the next field.
 */
void ParquetRowBuilder::nextField(const RtlFieldInfo *field)
{
    if (!field->name)
    {
        failx("Field name is empty.");
    }
    if (pathStack.size() > 0)
    {
        nextFromStruct(field);
        return;
    }
    arrayVisitor = std::make_shared<ParquetArrayVisitor>();
    auto column = resultRows->find(field->xpath ? field->xpath : field->name);
    if (column != resultRows->end())
    {
        reportIfFailure(column->second->Accept(arrayVisitor.get()));
        return;
    }
}

/**
 * @brief Logs what fields were bound to what index and increments the current parameter.
 *
 * @param field The field metadata.
 * @return The current parameter index.
 */
unsigned ParquetRecordBinder::checkNextParam(const RtlFieldInfo *field)
{
    if (logctx.queryTraceLevel() > 4)
        logctx.CTXLOG("Binding %s to %d", field->name, thisParam);
    return thisParam++;
}

/**
 * @brief Counts the fields in the row.
 *
 * @return int The number of fields.
 */
int ParquetRecordBinder::numFields()
{
    int count = 0;
    const RtlFieldInfo *const *fields = typeInfo->queryFields();
    assertex(fields);
    while (*fields++)
        count++;
    return count;
}

/**
 * @brief Writes the value to the Parquet file using the StreamWriter from the ParquetWriter class.
 *
 * @param len Number of chars in value.
 * @param value Pointer to value of parameter.
 * @param field RtlFieldInfo holds metadata about the field.
 * @param parquetWriter ParquetWriter object that holds the rapidjson::Value vector for building the rows
 */
void bindStringParam(unsigned len, const char *value, const RtlFieldInfo *field, std::shared_ptr<ParquetWriter> parquetWriter)
{
    size32_t utf8chars;
    rtlDataAttr utf8;
    rtlStrToUtf8X(utf8chars, utf8.refstr(), len, value);

    rapidjson::Value key = rapidjson::Value(field->name, jsonAlloc);
    rapidjson::Value val = rapidjson::Value(std::string(utf8.getstr(), rtlUtf8Size(utf8chars, utf8.getdata())), jsonAlloc);

    parquetWriter->addMember(key, val);
}

/**
 * @brief Calls the typeInfo member function process to write an ECL row to parquet.
 *
 * @param row Pointer to ECL row.
 */
void ParquetRecordBinder::processRow(const byte *row)
{
    thisParam = firstParam;
    typeInfo->process(row, row, &dummyField, *this);
}

/**
 * @brief Processes the field for its respective type, and adds the key-value pair to the current row.
 *
 * @param len Number of chars in value.
 * @param value Data to be written to the Parquet file.
 * @param field RtlFieldInfo holds metadeta about the field.
 */
void ParquetRecordBinder::processString(unsigned len, const char *value, const RtlFieldInfo *field)
{
    checkNextParam(field);
    bindStringParam(len, value, field, parquetWriter);
}

/**
 * @brief Processes the field for its respective type, and adds the key-value pair to the current row.
 *
 * @param value Data to be written to the Parquet file.
 * @param field RtlFieldInfo holds metadata about the field.
 */
void ParquetRecordBinder::processBool(bool value, const RtlFieldInfo *field)
{
    rapidjson::Value key = rapidjson::Value(field->name, jsonAlloc);
    rapidjson::Value val = rapidjson::Value(value);

    parquetWriter->addMember(key, val);
}

/**
 * @brief Processes the field for its respective type, and adds the key-value pair to the current row.
 *
 * @param len Number of chars in value.
 * @param value Data to be written to the Parquet file.
 * @param field RtlFieldInfo holds metadata about the field.
 */
void ParquetRecordBinder::processData(unsigned len, const void *value, const RtlFieldInfo *field)
{
    rapidjson::Value key = rapidjson::Value(field->name, jsonAlloc);
    rapidjson::Value val = rapidjson::Value((const char *)value, len, jsonAlloc);

    parquetWriter->addMember(key, val);
}

/**
 * @brief Processes the field for its respective type, and adds the key-value pair to the current row.
 *
 * @param value Data to be written to the Parquet file.
 * @param field RtlFieldInfo holds metadata about the field.
 */
void ParquetRecordBinder::processInt(__int64 value, const RtlFieldInfo *field)
{
    int64_t val = value;
    rapidjson::Value key = rapidjson::Value(field->name, jsonAlloc);
    rapidjson::Value num(val);

    parquetWriter->addMember(key, num);
}

/**
 * @brief Processes the field for its respective type, and adds the key-value pair to the current row.
 *
 * @param value Data to be written to the Parquet file.
 * @param field RtlFieldInfo holds metadata about the field.
 */
void ParquetRecordBinder::processUInt(unsigned __int64 value, const RtlFieldInfo *field)
{
    uint64_t val = value;
    rapidjson::Value key = rapidjson::Value(field->name, jsonAlloc);
    rapidjson::Value num(val);

    parquetWriter->addMember(key, num);
}

/**
 * @brief Processes the field for its respective type, and adds the key-value pair to the current row.
 *
 * @param value Data to be written to the Parquet file.
 * @param field RtlFieldInfo holds metadata about the field.
 */
void ParquetRecordBinder::processReal(double value, const RtlFieldInfo *field)
{
    rapidjson::Value key = rapidjson::Value(field->name, jsonAlloc);
    rapidjson::Value val = rapidjson::Value(value);

    parquetWriter->addMember(key, val);
}

/**
 * @brief Processes the field for its respective type, and adds the key-value pair to the current row.
 *
 * @param value Data to be written to the Parquet file.
 * @param digits Number of digits in decimal.
 * @param precision Number of digits of precision.
 * @param field RtlFieldInfo holds metadata about the field.
 */
void ParquetRecordBinder::processDecimal(const void *value, unsigned digits, unsigned precision, const RtlFieldInfo *field)
{
    Decimal val;
    size32_t bytes;
    rtlDataAttr decText;
    val.setDecimal(digits, precision, value);
    val.getStringX(bytes, decText.refstr());

    rapidjson::Value key = rapidjson::Value(field->name, jsonAlloc);
    rapidjson::Value dValue = rapidjson::Value(std::string(decText.getstr(), bytes), jsonAlloc);
    parquetWriter->addMember(key, dValue);
}

/**
 * @brief Processes the field for its respective type, and adds the key-value pair to the current row.
 *
 * @param chars Number of chars in the value.
 * @param value Data to be written to the Parquet file.
 * @param field RtlFieldInfo holds metadata about the field.
 */
void ParquetRecordBinder::processUnicode(unsigned chars, const UChar *value, const RtlFieldInfo *field)
{
    size32_t utf8chars;
    char *utf8;
    rtlUnicodeToUtf8X(utf8chars, utf8, chars, value);

    rapidjson::Value key = rapidjson::Value(field->name, jsonAlloc);
    rapidjson::Value val = rapidjson::Value(utf8, rtlUtf8Size(utf8chars, utf8), jsonAlloc);
    parquetWriter->addMember(key, val);
}

/**
 * @brief Processes the field for its respective type, and adds the key-value pair to the current row.
 *
 * @param len Length of QString
 * @param value Data to be written to the Parquet file.
 * @param field RtlFieldInfo holds metadata about the field.
 */
void ParquetRecordBinder::processQString(unsigned len, const char *value, const RtlFieldInfo *field)
{
    size32_t charCount;
    rtlDataAttr text;
    rtlQStrToStrX(charCount, text.refstr(), len, value);

    bindStringParam(charCount, text.getstr(), field, parquetWriter);
}

/**
 * @brief Processes the field for its respective type, and adds the key-value pair to the current row.
 *
 * @param chars Number of chars in the value.
 * @param value Data to be written to the Parquet file.
 * @param field RtlFieldInfo holds metadata about the field.
 */
void ParquetRecordBinder::processUtf8(unsigned chars, const char *value, const RtlFieldInfo *field)
{
    rapidjson::Value key = rapidjson::Value(field->name, jsonAlloc);
    rapidjson::Value val = rapidjson::Value(value, rtlUtf8Size(chars, value), jsonAlloc);

    parquetWriter->addMember(key, val);
}

/**
 * @brief Construct a new ParquetEmbedFunctionContext object and parses the options set by the user.
 *
 * @param _logctx Context logger for use with the ParquetRecordBinder ParquetDatasetBinder classes.
 * @param activityCtx Context about the Thor worker configuration.
 * @param options Pointer to the list of options that are passed into the Embed function.
 * @param _flags Should be zero if the embedded script is ok.
 */
ParquetEmbedFunctionContext::ParquetEmbedFunctionContext(const IContextLogger &_logctx, const IThorActivityContext *activityCtx, const char *options, unsigned _flags)
    : logctx(_logctx), scriptFlags(_flags)
{
    // Option Variables
    const char *option = "";            // Read(read), Read Parition(readpartition), Write(write), Write Partition(writepartition)
    const char *location = "";          // Full path to target location of where to write Parquet file/s. Can be a directory or filename.
    const char *destination = "";       // Full path to target location of where to read Parquet file/s. Can be a directory or filename.
    const char *partitionFields = "";   // Semicolon delimited values containing fields to partition files on
    __int64 maxRowCountInBatch = 40000; // Number of rows in the row groups when writing to Parquet files
    __int64 maxRowCountInTable = 40000; // Number of rows in the tables when converting Parquet columns to rows
    bool overwrite = false;             // If true overwrite file with no error. The default is false and will throw an error if the file already exists.
    arrow::Compression::type compressionOption = arrow::Compression::UNCOMPRESSED; // Compression option set by the user and defaults to UNCOMPRESSED.

    // Iterate through user options and save them
    StringArray inputOptions;
    inputOptions.appendList(options, ",");
    ForEachItemIn(idx, inputOptions)
    {
        const char *opt = inputOptions.item(idx);
        const char *val = strchr(opt, '=');
        if (val)
        {
            StringBuffer optName(val - opt, opt);
            val++;
            if (stricmp(optName, "option") == 0)
                option = val;
            else if (stricmp(optName, "location") == 0)
                location = val;
            else if (stricmp(optName, "destination") == 0)
                destination = val;
            else if (stricmp(optName, "MaxRowSize") == 0)
                maxRowCountInBatch = atoi(val);
            else if (stricmp(optName, "BatchSize") == 0)
                maxRowCountInTable = atoi(val);
            else if (stricmp(optName, "overwriteOpt") == 0)
                overwrite = clipStrToBool(val);
            else if (stricmp(optName, "compression") == 0)
            {
                if (strieq(val, "snappy"))
                    compressionOption = arrow::Compression::SNAPPY;
                else if (strieq(val, "gzip"))
                    compressionOption = arrow::Compression::GZIP;
                else if (strieq(val, "brotli"))
                    compressionOption = arrow::Compression::BROTLI;
                else if (strieq(val, "lz4"))
                    compressionOption = arrow::Compression::LZ4;
                else if (strieq(val, "lz4frame"))
                    compressionOption = arrow::Compression::LZ4_FRAME;
                else if (strieq(val, "lz4hadoop"))
                    compressionOption = arrow::Compression::LZ4_HADOOP;
                else if (strieq(val, "zstd"))
                    compressionOption = arrow::Compression::ZSTD;
                else if (strieq(val, "uncompressed"))
                    compressionOption = arrow::Compression::UNCOMPRESSED;
                else
                    failx("Unsupported compression type: %s", val);
            }
            else if (stricmp(optName, "partitionFields") == 0)
                partitionFields = val;
            else
                failx("Unknown option %s", optName.str());
        }
    }
    if (startsWithIgnoreCase(option, "read"))
    {
        parquetReader = std::make_shared<ParquetReader>(option, location, maxRowCountInTable, partitionFields, activityCtx);
    }
    else if (startsWithIgnoreCase(option, "write"))
    {
        parquetWriter = std::make_shared<ParquetWriter>(option, destination, maxRowCountInBatch, overwrite, compressionOption, partitionFields, activityCtx);
    }
    else
    {
        failx("Invalid read/write selection.");
    }
}

bool ParquetEmbedFunctionContext::getBooleanResult()
{
    UNIMPLEMENTED_X("Parquet Scalar Return Type BOOLEAN");
    return false;
}

void ParquetEmbedFunctionContext::getDataResult(size32_t &len, void *&result)
{
    UNIMPLEMENTED_X("Parquet Scalar Return Type DATA");
}

double ParquetEmbedFunctionContext::getRealResult()
{
    UNIMPLEMENTED_X("Parquet Scalar Return Type REAL");
    return 0.0;
}

__int64 ParquetEmbedFunctionContext::getSignedResult()
{
    UNIMPLEMENTED_X("Parquet Scalar Return Type SIGNED");
    return 0;
}

unsigned __int64 ParquetEmbedFunctionContext::getUnsignedResult()
{
    UNIMPLEMENTED_X("Parquet Scalar Return Type UNSIGNED");
    return 0;
}

void ParquetEmbedFunctionContext::getStringResult(size32_t &chars, char *&result)
{
    UNIMPLEMENTED_X("Parquet Scalar Return Type STRING");
}

void ParquetEmbedFunctionContext::getUTF8Result(size32_t &chars, char *&result)
{
    UNIMPLEMENTED_X("Parquet Scalar Return Type UTF8");
}

void ParquetEmbedFunctionContext::getUnicodeResult(size32_t &chars, UChar *&result)
{
    UNIMPLEMENTED_X("Parquet Scalar Return Type UNICODE");
}

void ParquetEmbedFunctionContext::getDecimalResult(Decimal &value)
{
    UNIMPLEMENTED_X("Parquet Scalar Return Type DECIMAL");
}

/**
 * @brief Return a Dataset read from Parquet to the user
 *
 * @param _resultAllocator Pointer to allocator for the engine.
 * @return Pointer to the memory allocated for the result.
 */
IRowStream *ParquetEmbedFunctionContext::getDatasetResult(IEngineRowAllocator *_resultAllocator)
{
    Owned<ParquetRowStream> parquetRowStream;
    parquetRowStream.setown(new ParquetRowStream(_resultAllocator, parquetReader));
    return parquetRowStream.getLink();
}

/**
 * @brief Return a Row read from Parquet to the user
 *
 * @param _resultAllocator Pointer to allocator for the engine.
 * @return Pointer to the memory allocated for the result.
 */
byte *ParquetEmbedFunctionContext::getRowResult(IEngineRowAllocator *_resultAllocator)
{
    Owned<ParquetRowStream> parquetRowStream;
    parquetRowStream.setown(new ParquetRowStream(_resultAllocator, parquetReader));
    return (byte *)parquetRowStream->nextRow();
}

size32_t ParquetEmbedFunctionContext::getTransformResult(ARowBuilder &rowBuilder)
{
    UNIMPLEMENTED_X("Parquet Transform Result");
    return 0;
}

/**
 * @brief Binds the values of a row to the with the ParquetWriter.
 *
 * @param name Name of the row field.
 * @param metaVal Metadata containing the type info of the row.
 * @param val The date for the row to be bound.
 */
void ParquetEmbedFunctionContext::bindRowParam(const char *name, IOutputMetaData &metaVal, const byte *val)
{
    ParquetRecordBinder binder(logctx, metaVal.queryTypeInfo(), nextParam, parquetWriter);
    binder.processRow(val);
    nextParam += binder.numFields();
}

/**
 * @brief Bind dataset parameter passed in by user.
 *
 * @param name name of the dataset.
 * @param metaVal Metadata holding typeinfo for the dataset.
 * @param val Input rowstream for binding the dataset data.
 */
void ParquetEmbedFunctionContext::bindDatasetParam(const char *name, IOutputMetaData &metaVal, IRowStream *val)
{
    if (oInputStream)
    {
        fail("At most one dataset parameter supported");
    }
    oInputStream.setown(new ParquetDatasetBinder(logctx, LINK(val), metaVal.queryTypeInfo(), parquetWriter, nextParam));
    nextParam += oInputStream->numFields();
}

void ParquetEmbedFunctionContext::bindBooleanParam(const char *name, bool val)
{
    UNIMPLEMENTED_X("Parquet Scalar Parameter type BOOLEAN");
}

void ParquetEmbedFunctionContext::bindDataParam(const char *name, size32_t len, const void *val)
{
    UNIMPLEMENTED_X("Parquet Scalar Parameter type DATA");
}

void ParquetEmbedFunctionContext::bindFloatParam(const char *name, float val)
{
    UNIMPLEMENTED_X("Parquet Scalar Parameter type FLOAT");
}

void ParquetEmbedFunctionContext::bindRealParam(const char *name, double val)
{
    UNIMPLEMENTED_X("Parquet Scalar Parameter type REAL");
}

void ParquetEmbedFunctionContext::bindSignedSizeParam(const char *name, int size, __int64 val)
{
    UNIMPLEMENTED_X("Parquet Scalar Parameter type SIGNED SIZE");
}

void ParquetEmbedFunctionContext::bindSignedParam(const char *name, __int64 val)
{
    UNIMPLEMENTED_X("Parquet Scalar Parameter type SIGNED");
}

void ParquetEmbedFunctionContext::bindUnsignedSizeParam(const char *name, int size, unsigned __int64 val)
{
    UNIMPLEMENTED_X("Parquet Scalar Parameter type UNSIGNED SIZE");
}

void ParquetEmbedFunctionContext::bindUnsignedParam(const char *name, unsigned __int64 val)
{
    UNIMPLEMENTED_X("Parquet Scalar Parameter type UNSIGNED");
}

void ParquetEmbedFunctionContext::bindStringParam(const char *name, size32_t len, const char *val)
{
    UNIMPLEMENTED_X("Parquet Scalar Parameter type STRING");
}

void ParquetEmbedFunctionContext::bindVStringParam(const char *name, const char *val)
{
    UNIMPLEMENTED_X("Parquet Scalar Parameter type VSTRING");
}

void ParquetEmbedFunctionContext::bindUTF8Param(const char *name, size32_t chars, const char *val)
{
    UNIMPLEMENTED_X("Parquet Scalar Parameter type UTF8");
}

void ParquetEmbedFunctionContext::bindUnicodeParam(const char *name, size32_t chars, const UChar *val)
{
    UNIMPLEMENTED_X("Parquet Scalar Parameter type UNICODE");
}

/**
 * @brief Compiles the embedded script passed in by the user. The script is placed inside the EMBED
 * and ENDEMBED block.
 *
 * @param chars The number of chars in the script.
 * @param script The embedded script for compilation.
 */
void ParquetEmbedFunctionContext::compileEmbeddedScript(size32_t chars, const char *script)
{
}

void ParquetEmbedFunctionContext::execute()
{
    if (oInputStream)
    {
        oInputStream->executeAll();
    }
    else
    {
        if (parquetReader)
        {
            reportIfFailure(parquetReader->processReadFile());
        }
        else
        {
            failx("Invalid read/write option.");
        }
    }
}

void ParquetEmbedFunctionContext::callFunction()
{
    execute();
}

unsigned ParquetEmbedFunctionContext::checkNextParam(const char *name)
{
    if (nextParam == numParams)
        failx("Too many parameters supplied: No matching $<name> placeholder for parameter %s", name);
    return nextParam++;
}

/**
 * @brief Gets the next ECL row.
 *
 * @return true If there is a row to process.
 * @return false If there are no rows left.
 */
bool ParquetDatasetBinder::bindNext()
{
    roxiemem::OwnedConstRoxieRow nextRow = (const byte *)input->ungroupedNextRow();
    if (!nextRow)
        return false;
    processRow((const byte *)nextRow.get()); // Bind the variables for the current row
    return true;
}

/**
 * @brief Binds all the rows of the dataset and executes the function.
 */
void ParquetDatasetBinder::executeAll()
{
    if (bindNext())
    {
        reportIfFailure(parquetWriter->openWriteFile());

        int i = 1;
        int maxRowCountInBatch = parquetWriter->getMaxRowSize();
        do
        {
            if (i % maxRowCountInBatch == 0)
            {
                parquetWriter->writeRecordBatch();
                jsonAlloc.Clear();
            }
            parquetWriter->updateRow();
            i++;
        }
        while (bindNext());

        i--;
        if (i % maxRowCountInBatch != 0)
        {
            parquetWriter->writeRecordBatch(i % maxRowCountInBatch);
            jsonAlloc.Clear();
        }
    }
}

/**
 * @brief Serves as the entry point for the HPCC Engine into the plugin and is how it obtains a
 * ParquetEmbedFunctionContext object for creating the query and executing it.
 */
class ParquetEmbedContext : public CInterfaceOf<IEmbedContext>
{
public:
    virtual IEmbedFunctionContext *createFunctionContext(unsigned flags, const char *options) override
    {
        return createFunctionContextEx(nullptr, nullptr, flags, options);
    }

    virtual IEmbedFunctionContext *createFunctionContextEx(ICodeContext *ctx, const IThorActivityContext *activityCtx, unsigned flags, const char *options) override
    {
        if (flags & EFimport)
        {
            UNSUPPORTED("IMPORT");
            return nullptr;
        }
        else
            return new ParquetEmbedFunctionContext(ctx ? ctx->queryContextLogger() : queryDummyContextLogger(), activityCtx, options, flags);
    }

    virtual IEmbedServiceContext *createServiceContext(const char *service, unsigned flags, const char *options) override
    {
        throwUnexpected();
        return nullptr;
    }
};

extern DECL_EXPORT IEmbedContext *getEmbedContext()
{
    return new ParquetEmbedContext();
}

extern DECL_EXPORT bool syntaxCheck(const char *script)
{
    return true;
}
}

MODULE_INIT(INIT_PRIORITY_STANDARD)
{
    return true;
}

MODULE_EXIT()
{
}
