#include <iostream>
// To understand what simdjson is doing, try defining SIMDJSON_VERBOSE_LOGGING
// before including simdjson.h
// #define SIMDJSON_VERBOSE_LOGGING 1
// See
// https://github.com/simdjson/simdjson/blob/master/doc/basics.md#performance-tips
#include "performancecounters/benchmarker.h"
#include "simdjson.h"
void pretty_print(size_t bytes, std::string name, event_aggregate agg) {
  printf("%-30s : ", name.c_str());
  printf(" %5.2f GB/s ", bytes / agg.fastest_elapsed_ns());
  if (collector.has_events()) {
    printf(" %5.2f GHz ", agg.fastest_cycles() / agg.fastest_elapsed_ns());
    printf(" %5.1f c/b ", agg.fastest_cycles() / bytes);
    printf(" %5.2f i/b ", agg.fastest_instructions() / bytes);
    printf(" %5.2f i/c ", agg.fastest_instructions() / agg.fastest_cycles());
  }
  printf("\n");
}

// return true on success, otherwise store the file content in output
// the result will include at least SIMDJSON_PADDING bytes of padding.
static bool read_file(const char *filename, std::vector<char> &output) {
  output.clear();
  // We intentionally mimick the way that Node.js reads files.
  std::FILE *fp = std::fopen(filename, "rb");
  if (fp == nullptr) {
    return false;
  }
  const size_t kBlockSize = 32 << 10; // 32kB
  int64_t offset = 0;
  // read the file in 32k blocks, for some reason
  do {
    // Performance hint: overallocate by SIMDJSON_PADDING so that we don't have
    // to copy.
    output.resize(offset + kBlockSize + simdjson::SIMDJSON_PADDING);
    size_t bytes_read = std::fread(output.data() + offset, 1, kBlockSize, fp);
    if (bytes_read == 0) {
      break;
    }
    offset += bytes_read;
  } while (true);
  output.resize(offset);
  if (std::fclose(fp) != 0) {
    return false;
  }
  if (offset == 0) {
    return false;
  }
  return true;
}

// return true on success, otherwise store the file content in output
// the result will include at least SIMDJSON_PADDING bytes of padding.
static bool two_pass_read_file(const char *filename,
                               std::vector<char> &output) {
  output.clear();
  // We intentionally mimick the way that Node.js reads files.
  std::FILE *fp = std::fopen(filename, "rb");
  if (fp == nullptr) {
    return false;
  }

  // Get the file size
  int ret = std::fseek(fp, 0, SEEK_END);
  if (ret < 0) {
    return false;
  }

  long llen = std::ftell(fp);
  if ((llen < 0) || (llen == LONG_MAX)) {
    return false;
  }
  output.resize(llen + simdjson::SIMDJSON_PADDING);

  // Read the padded_string
  std::rewind(fp);
  size_t bytes_read = std::fread(output.data(), 1, llen, fp);
  if (std::fclose(fp) != 0) {
    return false;
  }

  if (bytes_read != (size_t)llen) {
    return false;
  }
  output.resize(bytes_read);
  return true;
}
using result = std::tuple<bool, std::string, std::string, std::string,
                          std::string, std::string, bool, bool>;

result read_json(const std::vector<char> &input) {
  // Note: UTF-8 should not have a BOM as per the Unicode standard.
  size_t start = 0;
  if (input.size() >= 3 && 0 == memcmp(input.data(), "\xEF\xBB\xBF", 3)) {
    start = 3; // Skip UTF-8 BOM.
  }

  const size_t size = input.size() - start;
  simdjson::ondemand::parser parser;
  // Performance hint:
  // A padded_string_view is a string_view can be faster than a
  // padded_string because it does not require a copy!!!
  simdjson::padded_string_view json_string(input.data() + start, size,
                                           input.capacity() - start);
  simdjson::ondemand::document document;
  // Hint: we are going to require that the JSON document contain an object at
  // its root.
  simdjson::ondemand::object main_object;
  simdjson::error_code error = parser.iterate(json_string).get(document);

  // Hint: check for error before using the document.
  if (error) {
    std::cout << "error parsing  (can't get document)" << error << std::endl;
    return {};
  }
  error = document.get_object().get(main_object);
  if (error) {
    std::cout << "error parsing (can't get object) " << error << std::endl;
    return {};
  }

  std::string name, main, exports, imports;
  bool includes_keys = false;
  bool parse_exports = false;
  bool parse_imports = false;

  // Check for "name" field
  std::string_view name_value{};
  error = main_object["name"].get_string().get(name_value);
  if (!error) {
    name.assign(name_value);
    includes_keys = true;
  }

  // Check for "main" field
  std::string_view main_value{};
  error = main_object["main"].get_string().get(main_value);
  if (!error) {
    main.assign(main_value);
    includes_keys = true;
  }

  // Check for "exports" field
  // Performance hint: do not query "exports" twice
  // capture the value and reuse it.
  simdjson::ondemand::value exports_value;
  error = main_object["exports"].get(exports_value);
  if (!error) {
    simdjson::ondemand::json_type exports_json_type;
    error = exports_value.type().get(exports_json_type);
    if (!error) {
      std::string_view exports_value_view;
      switch (exports_json_type) {
      case simdjson::ondemand::json_type::object: {
        simdjson::ondemand::object exports_object;
        if (!exports_value.get_object().get(exports_object)) {
          if (!exports_object.raw_json().get(exports_value_view)) {
            exports.reserve(
                exports_value_view.size() +
                simdjson::SIMDJSON_PADDING); // pad so that later we can use
                                             // simdjson if needed
            exports.assign(exports_value_view);
            includes_keys = true;
            parse_exports = true;
          }
        }
        break;
      }
      case simdjson::ondemand::json_type::string: {
        if (!exports_value.get_string().get(exports_value_view)) {
          exports.assign(exports_value_view);
          includes_keys = true;
        }
        break;
      }
      default: {
        // do nothing
      }
      }
    }
  }

  // Performance hint: do not query "imports" twice
  // capture the value and reuse it.

  simdjson::ondemand::value imports_value;
  error = main_object["imports"].get(imports_value);
  if (!error) {

    // Check for "imports" field
    simdjson::ondemand::json_type imports_json_type;
    error = imports_value.type().get(imports_json_type);
    if (!error) {
      std::string_view imports_value_view;
      switch (imports_json_type) {
      case simdjson::ondemand::json_type::object: {
        simdjson::ondemand::object imports_object;
        if (!imports_value.get_object().get(imports_object)) {
          if (!imports_object.raw_json().get(imports_value_view)) {
            imports.reserve(
                imports_value_view.size() +
                simdjson::SIMDJSON_PADDING); // pad so that later we can use
                                             // simdjson if needed
            imports.assign(imports_value_view);
            includes_keys = true;
            parse_imports = true;
          }
        }
        break;
      }
      case simdjson::ondemand::json_type::string: {
        if (!imports_value.get_string().get(imports_value_view)) {
          imports.assign(imports_value_view);
          includes_keys = true;
        }
        break;
      }
      default: {
        // do nothing
      }
      }
    }
  }

  // Check for "type" field

  std::string_view type_value = "none";
  error = main_object["type"].get_string().get(type_value);
  if (!error) {
    // Ignore unknown types for forwards compatibility
    if (type_value != "module" && type_value != "commonjs") {
      type_value = "none";
    }
    includes_keys = true;
  }
  std::string type(type_value);

  // No need to optimize for the failed case, since this is highly unlikely.
  if (error != simdjson::error_code::NO_SUCH_FIELD &&
      error != simdjson::error_code::SUCCESS) {
    std::cout << "error parsing " << error << std::endl;
    return {};
  }
  return {includes_keys, name, main,          exports,
          imports,       type, parse_exports, parse_imports};
}

result fast_read_json(const std::vector<char> &input) {
  // Note: UTF-8 should not have a BOM as per the Unicode standard.
  size_t start = 0;
  if (input.size() >= 3 && 0 == memcmp(input.data(), "\xEF\xBB\xBF", 3)) {
    start = 3; // Skip UTF-8 BOM.
  }

  const size_t size = input.size() - start;
  simdjson::ondemand::parser parser;
  // Performance hint:
  // A padded_string_view is a string_view can be faster than a
  // padded_string because it does not require a copy!!!
  simdjson::padded_string_view json_string(input.data() + start, size,
                                           input.capacity() - start);
  simdjson::ondemand::document document;
  // Hint: we are going to require that the JSON document contain an object at
  // its root.
  simdjson::ondemand::object main_object;
  simdjson::error_code error = parser.iterate(json_string).get(document);

  // Hint: check for error before using the document.
  if (error) {
    std::cout << "error parsing " << std::endl;
    return {};
  }
  error = document.get_object().get(main_object);
  if (error) {
    std::cout << "error parsing " << std::endl;
    return {};
  }

  // Performance hint: instead of querying the field names one by one, scan the
  // document just once.

  std::string name, main, exports, imports;
  bool includes_keys = false;
  bool parse_exports = false;
  bool parse_imports = false;
  std::string_view type_value = "none";

  for (auto field : main_object) {
    simdjson::ondemand::raw_json_string field_key;
    error = field.key().get(field_key);
    if (error) {
      std::cout << "error parsing " << std::endl;
      return {};
    }
    if (field_key == "name") {
      simdjson::ondemand::value value;
      error = field.value().get(value);
      if (error) {
        std::cout << "error parsing " << std::endl;
        return {};
      }

      std::string_view name_value{};
      error = value.get_string().get(name_value);
      if (!error) {

        name.assign(name_value);
        includes_keys = true;
      }
    } else if (field_key == "main") {
      simdjson::ondemand::value value;
      error = field.value().get(value);
      if (error) {
        std::cout << "error parsing " << std::endl;
        return {};
      }
      std::string_view main_value{};
      error = value.get_string().get(main_value);
      if (!error) {
        main.assign(main_value);
        includes_keys = true;
      }
    } else if (field_key == "exports") {
      simdjson::ondemand::value value;
      error = field.value().get(value);
      if (error) {
        std::cout << "error parsing " << std::endl;
        return {};
      }
      simdjson::ondemand::json_type exports_json_type;
      error = value.type().get(exports_json_type);
      if (!error) {
        std::string_view exports_value;
        switch (exports_json_type) {
        case simdjson::ondemand::json_type::object: {
          simdjson::ondemand::object exports_object;
          if (!value.get_object().get(exports_object)) {
            if (!exports_object.raw_json().get(exports_value)) {
              exports.reserve(
                  exports_value.size() +
                  simdjson::SIMDJSON_PADDING); // pad so that later we can use
                                               // simdjson if needed
              exports.assign(exports_value);
              includes_keys = true;
              parse_exports = true;
            }
          }
          break;
        }
        case simdjson::ondemand::json_type::string: {
          if (!value.get_string().get(exports_value)) {
            exports.assign(exports_value);
            includes_keys = true;
          }
          break;
        }
        default: {
          // do nothing
        }
        }
      }
    } else if (field_key == "imports") {
      simdjson::ondemand::value value;
      error = field.value().get(value);
      if (error) {
        std::cout << "error parsing " << std::endl;
        return {};
      }
      simdjson::ondemand::json_type imports_json_type;
      error = value.type().get(imports_json_type);
      if (!error) {
        std::string_view imports_value;
        switch (imports_json_type) {
        case simdjson::ondemand::json_type::object: {
          simdjson::ondemand::object imports_object;
          if (!value.get_object().get(imports_object)) {
            if (!imports_object.raw_json().get(imports_value)) {
              imports.reserve(
                  imports_value.size() +
                  simdjson::SIMDJSON_PADDING); // pad so that later we can use
                                               // simdjson if needed
              imports.assign(imports_value);
              includes_keys = true;
              parse_imports = true;
            }
          }
          break;
        }
        case simdjson::ondemand::json_type::string: {
          if (!value.get_string().get(imports_value)) {
            imports.assign(imports_value);
            includes_keys = true;
          }
          break;
        }
        default: {
          // do nothing
        }
        }
      }
    } else if (field_key == "type") {
      simdjson::ondemand::value value;
      error = field.value().get(value);
      if (error) {
        std::cout << "error parsing " << std::endl;
        return {};
      }
      // Check for "type" field
      error = value.get_string().get(type_value);
      if (!error) {
        // Ignore unknown types for forwards compatibility
        if (type_value != "module" && type_value != "commonjs") {
          type_value = "none";
        }
        includes_keys = true;
      }
    } else {
      // skip unknown fields
    }
  }

  std::string type(type_value);

  return {includes_keys, name, main,          exports,
          imports,       type, parse_exports, parse_imports};
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cout << "Usage: " << argv[0] << " <jsonfile>" << std::endl;
    return EXIT_FAILURE;
  }
  const char *filename = argv[1];
  std::vector<char> json_content;
  if (!two_pass_read_file(filename, json_content)) {
    std::cout << "Could not read file " << filename << std::endl;
    return EXIT_FAILURE;
  }
  result r;
  pretty_print(json_content.size(), "read_file",
               bench([&json_content, &filename]() {
                 json_content.clear();
                 json_content.shrink_to_fit();
                 read_file(filename, json_content);
               }));
  pretty_print(json_content.size(), "two_pass_read_file",
               bench([&json_content, &filename]() {
                 json_content.clear();
                 json_content.shrink_to_fit();
                 two_pass_read_file(filename, json_content);
               }));

  pretty_print(json_content.size(), "read_json",
               bench([&json_content, &r]() { r = read_json(json_content); }));
  pretty_print(
      json_content.size(), "fast_read_json",
      bench([&json_content, &r]() { r = fast_read_json(json_content); }));
  pretty_print(json_content.size(), "file load + fast_read_json",
               bench([&json_content, &filename, &r]() {
                 json_content.clear();
                 read_file(filename, json_content);
                 r = fast_read_json(json_content);
               }));
  return EXIT_SUCCESS;
}
