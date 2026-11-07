// Trace reader implementation. Speed matters here: a long recording has millions of rows, so rows
// are parsed in place from one read of the file, without a string per cell.

#include "trace.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <limits>
#include <sstream>

namespace sessionscope {

namespace {

const double NOT_A_NUMBER = std::numeric_limits<double>::quiet_NaN();

// Everything in the file from `offset` on. Empty if the file is missing or has nothing new.
std::string read_from(const std::filesystem::path &path, std::size_t offset) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};

    const std::size_t file_size = static_cast<std::size_t>(file.tellg());
    if (file_size <= offset) return {};

    std::string text(file_size - offset, '\0');
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------------------- Table

Table::Table(const std::vector<std::string> &names) : names_(names), columns_(names.size()) {}

std::size_t Table::size() const { return columns_.empty() ? 0 : columns_[0].size(); }

const std::vector<double> &Table::operator[](std::string_view name) const {
    for (std::size_t i = 0; i < names_.size(); ++i) {
        if (names_[i] == name) return columns_[i];
    }
    throw std::out_of_range("Unknown column: " + std::string(name));
}

void Table::append_row(const std::vector<double> &values) {
    if (values.size() != columns_.size()) throw std::invalid_argument("Row size does not match column count");
    for (std::size_t i = 0; i < values.size(); ++i) columns_[i].push_back(values[i]);
}

void Table::drop_front(std::size_t rows) {
    for (auto &column : columns_) {
        column.erase(column.begin(), column.begin() + static_cast<std::ptrdiff_t>(std::min(rows, column.size())));
    }
}

// --------------------------------------------------------------------------------- parse_number

double parse_number(std::string_view text) {
    // from_chars is by far the fastest and handles every number a recorder writes.
    double fast_value = 0;
    const char *const text_end = text.data() + text.size();
    const auto fast = std::from_chars(text.data(), text_end, fast_value);
    if (fast.ec == std::errc{} && fast.ptr == text_end) return fast_value;

    // Anything else goes the slow way, which behaves like Python's float(): surrounding spaces,
    // a leading +, and out-of-range values that become infinity.
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);

    // strtod needs a terminated string. Every real number in a trace fits this buffer, and copying
    // to the stack is what keeps this free of allocations.
    char terminated[64];
    if (text.empty() || text.size() >= sizeof(terminated)) return NOT_A_NUMBER;
    std::memcpy(terminated, text.data(), text.size());
    terminated[text.size()] = '\0';

    char *end = nullptr;
    const double value = std::strtod(terminated, &end);
    const bool all_consumed = end == terminated + text.size();
    return all_consumed ? value : NOT_A_NUMBER;
}

// ----------------------------------------------------------------------------------- CsvFollower

CsvFollower::CsvFollower(const std::filesystem::path &path, const std::vector<std::string> &expected_columns)
    : path_(path), expected_(expected_columns) {}

void CsvFollower::read_header(const std::vector<std::string_view> &cells) {
    header_size_ = cells.size();
    index_.clear();
    std::string missing;
    for (const std::string &column : expected_) {
        const auto found = std::find(cells.begin(), cells.end(), column);
        if (found == cells.end()) {
            missing += (missing.empty() ? "" : ", ") + column;
        } else {
            index_.push_back(static_cast<std::size_t>(found - cells.begin()));
        }
    }
    if (!missing.empty()) throw std::runtime_error(path_.filename().string() + ": missing columns " + missing);
    header_read_ = true;
}

void CsvFollower::for_each_new_row(const RowCallback &on_row) {
    const std::string text = read_from(path_, offset_);
    const char *const begin = text.data();
    const char *const end = begin + text.size();

    std::vector<std::string_view> cells;   // reused for every row: no allocation after the first
    std::deque<std::string> unquoted;      // only quoted cells need their own text; a deque keeps it in place
    const char *position = begin;
    std::size_t consumed = 0;              // bytes up to the end of the last complete row

    while (position < end) {
        cells.clear();
        unquoted.clear();
        bool row_complete = false;

        while (position < end && !row_complete) {
            if (*position == '"') {
                // A quoted cell may hold commas, newlines and doubled quotes. As in Python's csv
                // module, anything after the closing quote still belongs to the cell.
                std::string &cell = unquoted.emplace_back();
                bool in_quotes = true;
                ++position;
                while (position < end && (in_quotes || (*position != ',' && *position != '\n'))) {
                    if (in_quotes && *position == '"') {
                        const bool doubled = position + 1 < end && position[1] == '"';
                        if (doubled) cell.push_back('"');
                        else in_quotes = false;
                        position += doubled ? 2 : 1;
                    } else {
                        cell.push_back(*position++);
                    }
                }
                cells.emplace_back(cell);
            } else {
                const char *const cell_start = position;
                while (position < end && *position != ',' && *position != '\n') ++position;
                cells.emplace_back(cell_start, static_cast<std::size_t>(position - cell_start));
            }

            if (position < end) {
                row_complete = *position == '\n';
                ++position;   // past the comma or the newline
            }
        }

        // Ran out of text inside a row: the writer is still writing it, so read it again next time.
        if (!row_complete) break;

        std::string_view &last_cell = cells.back();
        if (!last_cell.empty() && last_cell.back() == '\r') last_cell.remove_suffix(1);   // CRLF files

        consumed = static_cast<std::size_t>(position - begin);
        if (header_read_) on_row(cells);
        else read_header(cells);
    }
    offset_ += consumed;
}

std::vector<std::vector<std::string>> CsvFollower::read() {
    std::vector<std::vector<std::string>> rows;
    for_each_new_row([&](const std::vector<std::string_view> &cells) {
        std::vector<std::string> &row = rows.emplace_back();
        for (std::size_t column : index_) {
            row.emplace_back(column < cells.size() ? cells[column] : std::string_view{});
        }
    });
    return rows;
}

// ----------------------------------------------------------------------------------- TraceReader

TraceReader::TraceReader(const std::filesystem::path &folder)
    : folder_(folder),
      sensor_(SENSOR_COLUMNS),
      transport_(TRANSPORT_COLUMNS),
      host_(HOST_COLUMNS),
      sensor_follower_(folder / "sensor.csv", SENSOR_COLUMNS),
      transport_follower_(folder / "transport.csv", TRANSPORT_COLUMNS),
      host_follower_(folder / "host.csv", HOST_COLUMNS),
      events_follower_(folder / "events.csv", EVENT_COLUMNS) {}

void TraceReader::append_new_rows(CsvFollower &follower, Table &table) {
    std::vector<double> values;
    follower.for_each_new_row([&](const std::vector<std::string_view> &cells) {
        if (cells.size() < follower.header_size()) return;   // a damaged row: skip it rather than guess
        values.clear();
        for (std::size_t column : follower.index()) values.push_back(parse_number(cells[column]));
        table.append_row(values);
    });
}

std::size_t TraceReader::update() {
    std::ifstream meta_file(folder_ / "meta.json", std::ios::binary);
    if (meta_file) {
        std::ostringstream meta_text;
        meta_text << meta_file.rdbuf();
        meta_json_ = meta_text.str();
    }

    const std::size_t rows_before = sensor_.size();
    append_new_rows(sensor_follower_, sensor_);
    append_new_rows(transport_follower_, transport_);
    append_new_rows(host_follower_, host_);

    events_follower_.for_each_new_row([&](const std::vector<std::string_view> &cells) {
        if (cells.size() < 2) return;
        events_.push_back({parse_number(cells[0]), std::string(cells[1]),
                           cells.size() > 2 ? std::string(cells[2]) : std::string()});
    });
    return sensor_.size() - rows_before;
}

TraceReader load_trace(const std::filesystem::path &folder) {
    if (!std::filesystem::exists(folder / "sensor.csv")) {
        throw std::runtime_error(folder.string() + ": no sensor.csv here, so this is not a SessionScope trace folder");
    }
    TraceReader reader(folder);
    reader.update();
    return reader;
}

}  // namespace sessionscope
