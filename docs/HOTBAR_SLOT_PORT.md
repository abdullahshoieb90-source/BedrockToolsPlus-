# نقل ميزة Hotbar Slot إلى BedrockToolsPlus

> **الوضع:** نسخ (الميزة ما زالت موجودة في LeviLaunchroid كما هي).
> **المصدر:** `app/src/main/java/org/levimc/launcher/core/mods/inbuilt/` في هذا المستودع.
> **الهدف:** `abdullahshoieb90-source/BedrockToolsPlus-` (فرع `main`) — مود C++20 أصلي (native) للانشر.
> **الحالة:** الـ patch مُختبَر — يتطبق بنجاح على نسخة نظيفة من المستودع الهدف، وكل اختبارات الـ host بتنجح (بما فيها 27 فحص جديد).

## محتويات الحزمة

| الملف | الوصف |
|---|---|
| `bedrocktoolsplus-hotbarslots.patch` | patch جاهز يتطبق بـ `git apply` (7 ملفات، تم التحقق منه على نسخة نظيفة) |
| `hotbarslots.hpp` / `hotbarslots.cpp` | الموديول الجديد (يتحط في `src/modules/hud/`) |
| `hotbarslots_test.cpp` | اختبار host (يتحط في `tests/`) |

## التركيب (اختار طريقة واحدة)

### الطريقة 1: بالـ patch (الأسرع)

```bash
cd BedrockToolsPlus-
git apply /path/to/bedrocktoolsplus-hotbarslots.patch
./scripts/run_tests.sh   # لازم تشوف hotbarslots_test ناجح
```

الـ patch بيعمل كل حاجة: ملفين الموديول + الاختبار + التسجيل + القائمة + الـ README.

### الطريقة 2: نسخ يدوي

1. انسخ `hotbarslots.hpp` و `hotbarslots.cpp` إلى `src/modules/hud/`.
2. انسخ `hotbarslots_test.cpp` إلى `tests/`.
3. في `src/modules/ModuleRegistry.cpp`: ضيف سطر الـ include بعد `#include "hud/armorhud.hpp"`:
   ```cpp
   #include "hud/hotbarslots.hpp"
   ```
   وضيف سطر التسجيل بعد `registry.emplace<ArmorHudModule>();`:
   ```cpp
   registry.emplace<HotbarSlotsModule>();
   ```
4. في `src/launcher/ModuleMenu.cpp`: ضيف اسم الموديول لقائمة تحديث الأزرار (عشان تعديل الإعدادات يتطبق على الأزرار الظاهرة فورًا بدون إخفاء):
   ```cpp
   if (module_id == "bedrocktoolsplus.CommentKey" ||
       module_id == "bedrocktoolsplus.Command Hotkey" ||
       module_id == "bedrocktoolsplus.Hotbar Slots") {
   ```
5. في `scripts/run_tests.sh`: ضيف حالة `hotbarslots_test` (انسخها من الـ patch).
6. (اختياري) حدّث عدّاد الموديولات وقائمة الـ HUD في `README.md` (54 بدل 53 + `Hotbar Slots`).

> ملاحظة: ملفات `src/modules/**.cpp` بتتجمع تلقائيًا (glob في `xmake.lua`)، فمفيش حاجة تتعدل في إعدادات البناء.

## خريطة النقل (الأصل ← المنقول)

| LeviLaunchroid (Java + prebuilt native) | BedrockToolsPlus (C++ native) | ملاحظات |
|---|---|---|
| `HotbarSlotOverlay` (9 أزرار 1–9) | `syncOverlayButtons()` — زر `ButtonBuilder` لكل slot ظاهر | نفس ستايل إطار الـ Zoom/Command Hotkey، والرقم بقى label الزر |
| `sendSlotKey`: ضغط = key down، رفع = key up | `selectSlot()`: down عند الضغطة + up في أول `onFrame()` بعده | يضمن إن اللعبة تشوف الزر مضغوطًا لمدة frame كامل على الأقل |
| `MoreButtonsMod.sendKey('0'+slot)` | `platformSendKey()` عبر JNI لنفس الدالة `MoreButtonsMod.sendKey` | نفس مسار الإدخال الأصلي بالظبط |
| `isHotbarSlotEnabled(slot)` | `m_slot1` … `m_slot9` (toggles) | القيمة الافتراضية `true` زي الأصل |
| حجم الزر لكل slot (20–100dp) | `m_slotNSize` (float، نفس المدى) + `m_buttonScale` عام (0.5–2) | `sizeScale = (sizeDp / 52) × scale` — نفس معادلة Command Hotkey |
| شفافية الزر لكل slot | `m_slotNOpacity` + `m_buttonOpacity` | **مخزنة فقط حاليًا**: واجهة `ButtonBuilder` الحالية فيها حجم بس من غير شفافية (نفس وضع `m_buttonOpacity` في Command Hotkey) — الشفافية الفعلية من إعدادات اللانشر |
| `hotbar_item_icons` + `HotbarSlotMod.hasItem()` (مكتبة `libinbuiltmods.so` المقفولة) | `m_itemIcons` + `hotbarHasItem()` يقرأ ميموري الـ inventory مباشرة بنفس الـ offsets اللي بيستخدمها `InventoryAccess` | الـ slot المشغول رقمه بيبقى أبيض بدل الغامق؛ الأيقونات الحقيقية (rendering) محتاجة شغل إضافي — شوف "حدود معروفة" |
| `InbuiltModuleProvider` (تعريف المود + الإعدادات) | `loadConfig`/`saveConfig` — القائمة بتتولد تلقائيًا من مفاتيح الـ JSON | `m_slotN` toggle أب، و`m_slotNSize`/`m_slotNOpacity` تحته تلقائيًا (نفس قاعدة الـ prefix) |
| `ModIds.HOTBAR_SLOT` | اسم المود `"Hotbar Slots"` → ‏ID ‏`"bedrocktoolsplus.Hotbar Slots"` | الـ button IDs ثابتة من غير مسافات عشان اللانشر يحفظ أماكن الأزرار |

## إعدادات القائمة الجديدة (بتظهر تلقائيًا)

- **Item Icons** (toggle، افتراضي OFF زي الأصل) — تمييز الـ slots المشغولة.
- **Button Scale** (سلايدر 0.5–2) و **Button Opacity** (سلايدر 0–1).
- **Slot 1 … Slot 9** (toggles) — إظهار/إخفاء كل زر، وتحته **Slot N Size** و **Slot N Opacity**.

## حدود معروفة (بأمانة)

1. **لا أيقونات عناصر حقيقية على الأزرار بعد:** الأصل كان بيرسم أيقونة العنصر فوق الزر عبر مكتبة native مقفولة المصدر (`libinbuiltmods.so` — موجودة binary فقط حتى في المستودع الأم). البديل هنا تمييز لوني للـ slots المشغولة. لو حبيت الأيقونات الحقيقية، الطريق هو rendering داخل اللعبة زي ما `ArmorHudModule` بيعمل (item renderer + render hooks) — شغل كبير ومحدد بإصدار اللعبة.
2. **لا تمييز للـ slot المختار حاليًا:** اللعبة ممكن تغيّر الـ selected slot من السكرول أو الكيبورد، وقراءة الـ selected slot محتاجة offset غير مؤكد — عشان كده متعملتش (عمدًا، بدل ما نحط offset غلط يكرّش اللعبة).
3. **الضغط = tap مش hold:** واجهة أزرار اللانشر (`Click`/`Toggle`) مفيهاش press-and-hold، لكن اختيار slot أصلًا action لحظي فالـ tap هو الصح.
4. **الأزرار بتتسجل حتى والموديول مطفي** (نفس سلوك Command Hotkey) — الضغط عليها وهي مطفية مبيعملش حاجة. لو عايزها تختفي تمامًا زي الأصل، المستخدم يقدر يخفيها من HUD editor بتاع اللانشر.

## الاختبار

```bash
cd BedrockToolsPlus-
./scripts/run_tests.sh
```

هتشوف قسم `=== hotbarslots_test ===` ناجحًا (27 فحص: الأزرار الافتراضية، الإعدادات، حفظ/تحميل، تسلسل down/up، قراءة الـ inventory من buffer مُصنّع، التمييز والتحديث). باقي المجموعة لازم تفضل ناجحة زي ما كانت.

> البناء الكامل للأندرويد (`xmake`) محتاج NDK وبيئة اللعبة — الـ patch متأكد منه على مستوى الكود والـ host tests فقط، فجرّب الـ levipack على جهاز حقيقي قبل الإصدار.
