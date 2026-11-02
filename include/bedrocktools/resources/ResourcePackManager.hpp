#pragma once

#include <string>
#include <vector>

namespace bedrocktools::resources {

// يمسح ملفات الـ Resource Pack الموجودة داخل مجلد الـ Resource Pack الخاص بـ BedrockToolsPlus
// (الموجود في مجلد الموارد الخاص بالـ Launcher) ويرجع قائمة بأسماء الملفات (مثل .zip أو .mcpack)
// بحيث يتم تحميلها وتطبيقها على اللعبة الأصلية تلقائياً عند تشغيل BedrockToolsPlus.
std::vector<std::string> scanResourcePacks(const std::string& dir);

} // namespace bedrocktools::resources
