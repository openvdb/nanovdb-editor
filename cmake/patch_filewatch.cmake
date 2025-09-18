# CMake script to patch FileWatch.hpp to handle IN_MOVED_TO as Event::renamed_new

file(READ "FileWatch.hpp" FILEWATCH_HPP_CONTENT)

# Patch the inotify_add_watch mask to include IN_MOVED_TO
string(REGEX REPLACE
  "inotify_add_watch\\(([^,]+), ([^,]+), ([^)]+)\\)"
  "inotify_add_watch(\\1, \\2, IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_TO)"
  FILEWATCH_HPP_CONTENT
  "${FILEWATCH_HPP_CONTENT}"
)

# Patch the event handling to add IN_MOVED_TO -> renamed_new
string(REPLACE
  "else if (event->mask & IN_MODIFY)"
  "else if (event->mask & IN_MODIFY)\n{\n    parsed_information.emplace_back(StringType{ changed_file }, Event::modified);\n}\nelse if (event->mask & IN_MOVED_TO)\n{\n    parsed_information.emplace_back(StringType{ changed_file }, Event::renamed_new);\n}"
  FILEWATCH_HPP_CONTENT
  "${FILEWATCH_HPP_CONTENT}"
)

file(WRITE "FileWatch.hpp" "${FILEWATCH_HPP_CONTENT}")

message(STATUS "Successfully patched FileWatch.hpp")
