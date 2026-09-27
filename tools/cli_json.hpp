#pragma once

// Deterministic report rendering for the command line tool.
//
// Every command produces an ordered list of typed fields. The same list renders
// as `key=value` lines or as a flat JSON object, so the two output modes can
// never disagree about what was reported.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace power_capacity::cli {

class Report {
 public:
  void text(std::string_view name, std::string_view value);
  void integer(std::string_view name, std::int64_t value);
  void unsigned_integer(std::string_view name, std::uint64_t value);
  void boolean(std::string_view name, bool value);
  void unknown(std::string_view name);
  void watts(std::string_view name, std::int64_t milliwatts);
  /// Adds a field whose value is already rendered in watts.
  void watts_text(std::string_view name, std::string_view watts);

  bool empty() const noexcept { return fields_.empty(); }
  /// Appends every field of `other` after the fields already present.
  void append(const Report& other);
  std::string render_text() const;
  std::string render_json() const;

 private:
  struct Field {
    std::string name;
    std::string value;
    bool quoted = false;
    bool null_value = false;
  };
  std::vector<Field> fields_;
};

/// Escapes a UTF-8 string for JSON output.
std::string json_escape(std::string_view value);

}  // namespace power_capacity::cli
