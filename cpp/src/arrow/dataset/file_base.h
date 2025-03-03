// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// This API is EXPERIMENTAL.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "arrow/buffer.h"
#include "arrow/dataset/dataset.h"
#include "arrow/dataset/partition.h"
#include "arrow/dataset/scanner.h"
#include "arrow/dataset/type_fwd.h"
#include "arrow/dataset/visibility.h"
#include "arrow/filesystem/filesystem.h"
#include "arrow/io/file.h"
#include "arrow/util/compression.h"

namespace arrow {

namespace dataset {

/// \defgroup dataset-file-formats File formats for reading and writing datasets
/// \defgroup dataset-filesystem File system datasets
///
/// @{

/// \brief The path and filesystem where an actual file is located or a buffer which can
/// be read like a file
class ARROW_DS_EXPORT FileSource {
 public:
  FileSource(std::string path, std::shared_ptr<fs::FileSystem> filesystem,
             Compression::type compression = Compression::UNCOMPRESSED)
      : file_info_(std::move(path)),
        filesystem_(std::move(filesystem)),
        compression_(compression) {}

  FileSource(fs::FileInfo info, std::shared_ptr<fs::FileSystem> filesystem,
             Compression::type compression = Compression::UNCOMPRESSED)
      : file_info_(std::move(info)),
        filesystem_(std::move(filesystem)),
        compression_(compression) {}

  explicit FileSource(std::shared_ptr<Buffer> buffer,
                      Compression::type compression = Compression::UNCOMPRESSED)
      : buffer_(std::move(buffer)), compression_(compression) {}

  using CustomOpen = std::function<Result<std::shared_ptr<io::RandomAccessFile>>()>;
  explicit FileSource(CustomOpen open) : custom_open_(std::move(open)) {}

  using CustomOpenWithCompression =
      std::function<Result<std::shared_ptr<io::RandomAccessFile>>(Compression::type)>;
  explicit FileSource(CustomOpenWithCompression open_with_compression,
                      Compression::type compression = Compression::UNCOMPRESSED)
      : custom_open_(std::bind(std::move(open_with_compression), compression)),
        compression_(compression) {}

  explicit FileSource(std::shared_ptr<io::RandomAccessFile> file,
                      Compression::type compression = Compression::UNCOMPRESSED)
      : custom_open_([=] { return ToResult(file); }), compression_(compression) {}

  FileSource() : custom_open_(CustomOpen{&InvalidOpen}) {}

  static std::vector<FileSource> FromPaths(const std::shared_ptr<fs::FileSystem>& fs,
                                           std::vector<std::string> paths) {
    std::vector<FileSource> sources;
    for (auto&& path : paths) {
      sources.emplace_back(std::move(path), fs);
    }
    return sources;
  }

  /// \brief Return the type of raw compression on the file, if any.
  Compression::type compression() const { return compression_; }

  /// \brief Return the file path, if any. Only valid when file source wraps a path.
  const std::string& path() const {
    static std::string buffer_path = "<Buffer>";
    static std::string custom_open_path = "<Buffer>";
    return filesystem_ ? file_info_.path() : buffer_ ? buffer_path : custom_open_path;
  }

  /// \brief Return the filesystem, if any. Otherwise returns nullptr
  const std::shared_ptr<fs::FileSystem>& filesystem() const { return filesystem_; }

  /// \brief Return the buffer containing the file, if any. Otherwise returns nullptr
  const std::shared_ptr<Buffer>& buffer() const { return buffer_; }

  /// \brief Get a RandomAccessFile which views this file source
  Result<std::shared_ptr<io::RandomAccessFile>> Open() const;

  /// \brief Get an InputStream which views this file source (and decompresses if needed)
  /// \param[in] compression If nullopt, guess the compression scheme from the
  ///     filename, else decompress with the given codec
  Result<std::shared_ptr<io::InputStream>> OpenCompressed(
      util::optional<Compression::type> compression = util::nullopt) const;

 private:
  static Result<std::shared_ptr<io::RandomAccessFile>> InvalidOpen() {
    return Status::Invalid("Called Open() on an uninitialized FileSource");
  }

  fs::FileInfo file_info_;
  std::shared_ptr<fs::FileSystem> filesystem_;
  std::shared_ptr<Buffer> buffer_;
  CustomOpen custom_open_;
  Compression::type compression_ = Compression::UNCOMPRESSED;
};

/// \brief Base class for file format implementation
class ARROW_DS_EXPORT FileFormat : public std::enable_shared_from_this<FileFormat> {
 public:
  /// Options affecting how this format is scanned.
  ///
  /// The options here can be overridden at scan time.
  std::shared_ptr<FragmentScanOptions> default_fragment_scan_options;

  virtual ~FileFormat() = default;

  /// \brief The name identifying the kind of file format
  virtual std::string type_name() const = 0;

  virtual bool Equals(const FileFormat& other) const = 0;

  /// \brief Indicate if the FileSource is supported/readable by this format.
  virtual Result<bool> IsSupported(const FileSource& source) const = 0;

  /// \brief Return the schema of the file if possible.
  virtual Result<std::shared_ptr<Schema>> Inspect(const FileSource& source) const = 0;

  /// \brief Open a FileFragment for scanning.
  /// May populate lazy properties of the FileFragment.
  virtual Result<ScanTaskIterator> ScanFile(
      const std::shared_ptr<ScanOptions>& options,
      const std::shared_ptr<FileFragment>& file) const = 0;

  virtual Result<RecordBatchGenerator> ScanBatchesAsync(
      const std::shared_ptr<ScanOptions>& options,
      const std::shared_ptr<FileFragment>& file) const;
  virtual Future<util::optional<int64_t>> CountRows(
      const std::shared_ptr<FileFragment>& file, compute::Expression predicate,
      const std::shared_ptr<ScanOptions>& options);

  /// \brief Open a fragment
  virtual Result<std::shared_ptr<FileFragment>> MakeFragment(
      FileSource source, compute::Expression partition_expression,
      std::shared_ptr<Schema> physical_schema);

  /// \brief Create a FileFragment for a FileSource.
  Result<std::shared_ptr<FileFragment>> MakeFragment(
      FileSource source, compute::Expression partition_expression);

  /// \brief Create a FileFragment for a FileSource.
  Result<std::shared_ptr<FileFragment>> MakeFragment(
      FileSource source, std::shared_ptr<Schema> physical_schema = NULLPTR);

  /// \brief Create a writer for this format.
  virtual Result<std::shared_ptr<FileWriter>> MakeWriter(
      std::shared_ptr<io::OutputStream> destination, std::shared_ptr<Schema> schema,
      std::shared_ptr<FileWriteOptions> options,
      fs::FileLocator destination_locator) const = 0;

  /// \brief Get default write options for this format.
  virtual std::shared_ptr<FileWriteOptions> DefaultWriteOptions() = 0;
};

/// \brief A Fragment that is stored in a file with a known format
class ARROW_DS_EXPORT FileFragment : public Fragment {
 public:
  Result<ScanTaskIterator> Scan(std::shared_ptr<ScanOptions> options) override;
  Result<RecordBatchGenerator> ScanBatchesAsync(
      const std::shared_ptr<ScanOptions>& options) override;
  Future<util::optional<int64_t>> CountRows(
      compute::Expression predicate,
      const std::shared_ptr<ScanOptions>& options) override;

  std::string type_name() const override { return format_->type_name(); }
  std::string ToString() const override { return source_.path(); };

  const FileSource& source() const { return source_; }
  const std::shared_ptr<FileFormat>& format() const { return format_; }

 protected:
  FileFragment(FileSource source, std::shared_ptr<FileFormat> format,
               compute::Expression partition_expression,
               std::shared_ptr<Schema> physical_schema)
      : Fragment(std::move(partition_expression), std::move(physical_schema)),
        source_(std::move(source)),
        format_(std::move(format)) {}

  Result<std::shared_ptr<Schema>> ReadPhysicalSchemaImpl() override;

  FileSource source_;
  std::shared_ptr<FileFormat> format_;

  friend class FileFormat;
};

/// \brief A Dataset of FileFragments.
///
/// A FileSystemDataset is composed of one or more FileFragment. The fragments
/// are independent and don't need to share the same format and/or filesystem.
class ARROW_DS_EXPORT FileSystemDataset : public Dataset {
 public:
  /// \brief Create a FileSystemDataset.
  ///
  /// \param[in] schema the schema of the dataset
  /// \param[in] root_partition the partition expression of the dataset
  /// \param[in] format the format of each FileFragment.
  /// \param[in] filesystem the filesystem of each FileFragment, or nullptr if the
  ///            fragments wrap buffers.
  /// \param[in] fragments list of fragments to create the dataset from.
  /// \param[in] partitioning the Partitioning object in case the dataset is created
  ///            with a known partitioning (e.g. from a discovered partitioning
  ///            through a DatasetFactory), or nullptr if not known.
  ///
  /// Note that fragments wrapping files resident in differing filesystems are not
  /// permitted; to work with multiple filesystems use a UnionDataset.
  ///
  /// \return A constructed dataset.
  static Result<std::shared_ptr<FileSystemDataset>> Make(
      std::shared_ptr<Schema> schema, compute::Expression root_partition,
      std::shared_ptr<FileFormat> format, std::shared_ptr<fs::FileSystem> filesystem,
      std::vector<std::shared_ptr<FileFragment>> fragments,
      std::shared_ptr<Partitioning> partitioning = NULLPTR);

  /// \brief Write a dataset.
  static Status Write(const FileSystemDatasetWriteOptions& write_options,
                      std::shared_ptr<Scanner> scanner);

  /// \brief Return the type name of the dataset.
  std::string type_name() const override { return "filesystem"; }

  /// \brief Replace the schema of the dataset.
  Result<std::shared_ptr<Dataset>> ReplaceSchema(
      std::shared_ptr<Schema> schema) const override;

  /// \brief Return the path of files.
  std::vector<std::string> files() const;

  /// \brief Return the format.
  const std::shared_ptr<FileFormat>& format() const { return format_; }

  /// \brief Return the filesystem. May be nullptr if the fragments wrap buffers.
  const std::shared_ptr<fs::FileSystem>& filesystem() const { return filesystem_; }

  /// \brief Return the partitioning. May be nullptr if the dataset was not constructed
  /// with a partitioning.
  const std::shared_ptr<Partitioning>& partitioning() const { return partitioning_; }

  std::string ToString() const;

 protected:
  struct FragmentSubtrees;

  explicit FileSystemDataset(std::shared_ptr<Schema> schema)
      : Dataset(std::move(schema)) {}

  FileSystemDataset(std::shared_ptr<Schema> schema,
                    compute::Expression partition_expression)
      : Dataset(std::move(schema), partition_expression) {}

  Result<FragmentIterator> GetFragmentsImpl(compute::Expression predicate) override;

  void SetupSubtreePruning();

  std::shared_ptr<FileFormat> format_;
  std::shared_ptr<fs::FileSystem> filesystem_;
  std::vector<std::shared_ptr<FileFragment>> fragments_;
  std::shared_ptr<Partitioning> partitioning_;

  std::shared_ptr<FragmentSubtrees> subtrees_;
};

/// \brief Options for writing a file of this format.
class ARROW_DS_EXPORT FileWriteOptions {
 public:
  virtual ~FileWriteOptions() = default;

  const std::shared_ptr<FileFormat>& format() const { return format_; }

  std::string type_name() const { return format_->type_name(); }

 protected:
  explicit FileWriteOptions(std::shared_ptr<FileFormat> format)
      : format_(std::move(format)) {}

  std::shared_ptr<FileFormat> format_;
};

/// \brief A writer for this format.
class ARROW_DS_EXPORT FileWriter {
 public:
  virtual ~FileWriter() = default;

  /// \brief Write the given batch.
  virtual Status Write(const std::shared_ptr<RecordBatch>& batch) = 0;

  /// \brief Write all batches from the reader.
  Status Write(RecordBatchReader* batches);

  /// \brief Indicate that writing is done.
  virtual Status Finish();

  const std::shared_ptr<FileFormat>& format() const { return options_->format(); }
  const std::shared_ptr<Schema>& schema() const { return schema_; }
  const std::shared_ptr<FileWriteOptions>& options() const { return options_; }
  const fs::FileLocator& destination() const { return destination_locator_; }

 protected:
  FileWriter(std::shared_ptr<Schema> schema, std::shared_ptr<FileWriteOptions> options,
             std::shared_ptr<io::OutputStream> destination,
             fs::FileLocator destination_locator)
      : schema_(std::move(schema)),
        options_(std::move(options)),
        destination_(std::move(destination)),
        destination_locator_(std::move(destination_locator)) {}

  virtual Status FinishInternal() = 0;

  std::shared_ptr<Schema> schema_;
  std::shared_ptr<FileWriteOptions> options_;
  std::shared_ptr<io::OutputStream> destination_;
  fs::FileLocator destination_locator_;
};

/// \brief Controls what happens if files exist in an output directory during a dataset
/// write
enum ExistingDataBehavior : int8_t {
  /// Deletes all files in a directory the first time that directory is encountered
  kDeleteMatchingPartitions,
  /// Ignores existing files, overwriting any that happen to have the same name as an
  /// output file
  kOverwriteOrIgnore,
  /// Returns an error if there are any files or subdirectories in the output directory
  kError,
};

/// \brief Options for writing a dataset.
struct ARROW_DS_EXPORT FileSystemDatasetWriteOptions {
  /// Options for individual fragment writing.
  std::shared_ptr<FileWriteOptions> file_write_options;

  /// FileSystem into which a dataset will be written.
  std::shared_ptr<fs::FileSystem> filesystem;

  /// Root directory into which the dataset will be written.
  std::string base_dir;

  /// Partitioning used to generate fragment paths.
  std::shared_ptr<Partitioning> partitioning;

  /// Maximum number of partitions any batch may be written into, default is 1K.
  int max_partitions = 1024;

  /// Template string used to generate fragment basenames.
  /// {i} will be replaced by an auto incremented integer.
  std::string basename_template;

  /// If greater than 0 then this will limit the maximum number of files that can be left
  /// open. If an attempt is made to open too many files then the least recently used file
  /// will be closed.  If this setting is set too low you may end up fragmenting your data
  /// into many small files.
  uint32_t max_open_files = 1024;

  /// If greater than 0 then this will limit how many rows are placed in any single file.
  /// Otherwise there will be no limit and one file will be created in each output
  /// directory unless files need to be closed to respect max_open_files
  uint64_t max_rows_per_file = 0;

  /// Controls what happens if an output directory already exists.
  ExistingDataBehavior existing_data_behavior = kError;

  /// Callback to be invoked against all FileWriters before
  /// they are finalized with FileWriter::Finish().
  std::function<Status(FileWriter*)> writer_pre_finish = [](FileWriter*) {
    return Status::OK();
  };

  /// Callback to be invoked against all FileWriters after they have
  /// called FileWriter::Finish().
  std::function<Status(FileWriter*)> writer_post_finish = [](FileWriter*) {
    return Status::OK();
  };

  const std::shared_ptr<FileFormat>& format() const {
    return file_write_options->format();
  }
};

/// \brief Wraps FileSystemDatasetWriteOptions for consumption as compute::ExecNodeOptions
class ARROW_DS_EXPORT WriteNodeOptions : public compute::ExecNodeOptions {
 public:
  explicit WriteNodeOptions(FileSystemDatasetWriteOptions options,
                            std::shared_ptr<Schema> schema)
      : write_options(std::move(options)), schema(std::move(schema)) {}

  FileSystemDatasetWriteOptions write_options;
  std::shared_ptr<Schema> schema;
};

/// @}

namespace internal {
ARROW_DS_EXPORT void InitializeDatasetWriter(
    arrow::compute::ExecFactoryRegistry* registry);
}

}  // namespace dataset
}  // namespace arrow
