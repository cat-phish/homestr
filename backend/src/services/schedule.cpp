#include "homestr/services/schedule.hpp"

#include <bitset>
#include <cctype>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>

#include <croncpp.h>
#include <date/tz.h>

namespace homestr {
namespace {

struct ParsedSchedule {
  std::bitset<60> minutes;
  std::bitset<24> hours;
  std::bitset<31> days;
  std::bitset<12> months;
  std::bitset<7> weekdays;
  const date::time_zone* zone;

  explicit ParsedSchedule(const nlohmann::json& trigger) {
    const std::string expression = trigger.at("cron").get<std::string>();
    if (expression.empty() || expression.size() > 256) {
      throw std::invalid_argument("Invalid cron");
    }
    std::istringstream fields(expression);
    std::string field;
    int count = 0;
    while (fields >> field) {
      ++count;
      for (std::size_t index = 0; index < field.size();) {
        if (std::isdigit(static_cast<unsigned char>(field[index])) != 0) {
          std::size_t end = index;
          while (end < field.size() && std::isdigit(static_cast<unsigned char>(field[end])) != 0) {
            ++end;
          }
          // Prevent croncpp's uint8 conversion from wrapping oversized values.
          if (std::stoul(field.substr(index, end - index)) > 60 ||
              (index > 0 && field[index - 1] == '/' &&
               std::stoul(field.substr(index, end - index)) == 0)) {
            throw std::invalid_argument("Invalid cron value");
          }
          index = end;
        } else {
          if (std::string{"*,-/"}.find(field[index]) == std::string::npos) {
            throw std::invalid_argument("Invalid cron character");
          }
          ++index;
        }
      }
    }
    if (count != 5) {
      throw std::invalid_argument("Use five cron fields");
    }
    const auto parsed = cron::make_cron("0 " + expression);
    std::istringstream bits(cron::to_string(parsed));
    std::string seconds;
    bits >> seconds >> minutes >> hours >> days >> months >> weekdays;
    zone = date::locate_zone(trigger.value("timezone", std::string{"UTC"}));
  }

  bool matches(date::local_days day) const {
    const date::year_month_day calendar{day};
    return months.test(static_cast<unsigned>(calendar.month()) - 1) &&
           days.test(static_cast<unsigned>(calendar.day()) - 1) &&
           weekdays.test(date::weekday{day}.c_encoding());
  }
};

}  // namespace

std::int64_t Schedule::now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string Schedule::format(std::int64_t instant) {
  return date::format("%FT%TZ", date::sys_seconds{std::chrono::seconds{instant}});
}

bool Schedule::valid(const nlohmann::json& trigger) {
  return next(trigger, now()).has_value();
}

bool Schedule::due(const nlohmann::json& trigger, std::int64_t instant) {
  try {
    const ParsedSchedule parsed{trigger};
    const auto minute =
        date::floor<std::chrono::minutes>(date::sys_seconds{std::chrono::seconds{instant}});
    const auto local = parsed.zone->to_local(minute);
    const auto day = date::floor<date::days>(local);
    const date::hh_mm_ss time{local - day};
    return parsed.matches(day) &&
           parsed.hours.test(static_cast<std::size_t>(time.hours().count())) &&
           parsed.minutes.test(static_cast<std::size_t>(time.minutes().count())) &&
           parsed.zone->to_sys(local, date::choose::earliest) == minute;
  } catch (const std::exception&) {
    return false;
  }
}

std::optional<std::int64_t> Schedule::next(const nlohmann::json& trigger, std::int64_t after) {
  try {
    const ParsedSchedule parsed{trigger};
    const auto local = parsed.zone->to_local(date::sys_seconds{std::chrono::seconds{after}});
    const auto firstDay = date::floor<date::days>(local);
    for (int offset = 0; offset <= 366 * 5; ++offset) {
      const auto day = firstDay + date::days{offset};
      if (!parsed.matches(day)) {
        continue;
      }
      for (std::size_t hour = 0; hour < 24; ++hour) {
        if (!parsed.hours.test(hour)) {
          continue;
        }
        for (std::size_t minute = 0; minute < 60; ++minute) {
          if (!parsed.minutes.test(minute)) {
            continue;
          }
          const auto candidate = day + std::chrono::hours{static_cast<int>(hour)} +
                                 std::chrono::minutes{static_cast<int>(minute)};
          if (parsed.zone->get_info(candidate).result == date::local_info::nonexistent) {
            continue;
          }
          const auto utc = parsed.zone->to_sys(candidate, date::choose::earliest);
          const auto epoch =
              std::chrono::duration_cast<std::chrono::seconds>(utc.time_since_epoch()).count();
          if (epoch > after) {
            return epoch;
          }
        }
      }
    }
  } catch (const std::exception&) {
  }
  return std::nullopt;
}

}  // namespace homestr
