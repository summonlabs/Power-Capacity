#include "cli_json.hpp"

#include <cstdio>
#include <string>

namespace power_capacity::cli {
namespace {

std::string format_milliwatts(std::int64_t milliwatts) {
  const bool negative = milliwatts < 0;
  const std::uint64_t magnitude =
      negative ? (~static_cast<std::uint64_t>(milliwatts) + 1ULL)
               : static_cast<std::uint64_t>(milliwatts);
  const std::uint64_t whole = magnitude / 1000ULL;
  const std::uint64_t fraction = magnitude % 1000ULL;
  std::string text;
  if (negative) {
    text.push_back('-');
  }
  text += std::to_string(whole);
  text.push_back('.');
  for (int index = 2; index >= 0; --index) {
    const std::uint64_t divisor = (index == 2) ? 100ULL : (index == 1 ? 10ULL : 1ULL);
    text.push_back(static_cast<char>('0' + ((fraction / divisor) % 10ULL)));
  }
  return text;
}

}  // namespace

void Report::text(std::string_view name, std::string_view value) {
  fields_.push_back(Field{std::string(name), std::string(value), true, false});
}

void Report::integer(std::string_view name, std::int64_t value) {
  fields_.push_back(Field{std::string(name), std::to_string(value), false, false});
}

void Report::unsigned_integer(std::string_view name, std::uint64_t value) {
  fields_.push_back(Field{std::string(name), std::to_string(value), false, false});
}

void Report::boolean(std::string_view name, bool value) {
  fields_.push_back(Field{std::string(name), value ? "true" : "false", false, false});
}

void Report::unknown(std::string_view name) {
  fields_.push_back(Field{std::string(name), "unknown", true, true});
}

void Report::watts(std::string_view name, std::int64_t milliwatts) {
  fields_.push_back(Field{std::string(name), format_milliwatts(milliwatts), false, false});
}

void Report::watts_text(std::string_view name, std::string_view watts) {
  fields_.push_back(Field{std::string(name), std::string(watts), false, false});
}

void Report::append(const Report& other) {
  fields_.insert(fields_.end(), other.fields_.begin(), other.fields_.end());
}

std::string Report::render_text() const {
  std::string output;
  for (const Field& field : fields_) {
    output += field.name;
    output.push_back('=');
    output += field.value;
    output.push_back('\n');
  }
  return output;
}

std::string Report::render_json() const {
  std::string output = "{\n";
  for (std::size_t index = 0; index < fields_.size(); ++index) {
    const Field& field = fields_[index];
    output += "  \"";
    output += json_escape(field.name);
    output += "\": ";
    if (field.null_value) {
      output += "null";
    } else if (field.quoted) {
      output.push_back('"');
      output += json_escape(field.value);
      output.push_back('"');
    } else {
      output += field.value;
    }
    if (index + 1 < fields_.size()) {
      output.push_back(',');
    }
    output.push_back('\n');
  }
  output += "}\n";
  return output;
}

std::string json_escape(std::string_view value) {
  std::string output;
  output.reserve(value.size() + 8);
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    switch (character) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      default:
        if (byte < 0x20) {
          char buffer[8] = {};
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(byte));
          output += buffer;
        } else {
          output.push_back(character);
        }
        break;
    }
  }
  return output;
}

}  // namespace power_capacity::cli
