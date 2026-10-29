# XeroxClient — Minecraft Bedrock Edition Client Pack

![version](https://img.shields.io/badge/version-1.0.2-blue)
![minecraft](https://img.shields.io/badge/Minecraft%20Bedrock-1.18.30%2B-green)
![type](https://img.shields.io/badge/type-Resource%20Pack-lightgrey)

> مشروع **XeroxClient v1.0.2 (Official)** بعد فك ضغط ملف `.mcpack` الأصلي إلى ملفات مصدرية قابلة للتعديل والبناء من جديد.
>
> Source project for **XeroxClient v1.0.2**, created by unpacking the original
> `XeroxClient - v1.0.2 Official.mcpack.zip` archive into an editable, buildable project.

## تحويله إلى مستودع مستقل | Making it a standalone repository

هذا المجلد مستقل تمامًا وجاهز لأن يكون مستودع GitHub خاصًا به. لتحويله:

```bash
# من جذر هذا المجلد
git init -b main
git add -A
git commit -m "Initial commit: XeroxClient v1.0.2 resource pack"
gh repo create XeroxClient --private --source=. --remote=origin --push
```

> ملف `.github/workflows/build.yml` داخل هذا المجلد يعمل تلقائيًا بمجرد أن يصبح
> المجلد مستودعًا مستقلًا (GitHub يتجاهل الـ workflows الموجودة في مجلدات فرعية).
> If the folder is converted to its own repository, the packaged
> `.github/workflows/build.yml` becomes active automatically.

---

## نظرة عامة | Overview

| العنصر | القيمة |
| --- | --- |
| الاسم | `§l§bXerox§rClient` |
| الإصدار | `1.0.2` |
| نوع الحزمة | Resource Pack (Client Pack) — حزمة موارد |
| أقل إصدار لمحرك اللعبة | Minecraft Bedrock `1.18.30` |
| UUID الحزمة | `1B29F76B-A621-42D3-A1FB-02282B268865` |
| UUID الوحدة | `1D7C96D0-03B9-45A8-B335-5BAFDAD0279E` |
| المؤلف | **Greninja** (Bilibili: `Greninja_`) |
| عدد الملفات | 539 ملفًا (~11 ميجابايت) |

الحزمة لا تحتوي على أي أكواد برمجية تنفيذية — كل شيء مبني على ملفات JSON،
والنسيج (Textures)، والخطوط (Fonts)، والشيدرات (Materials)، والأصوات (Sounds).

---

## المميزات | Features

مأخوذة من `manifest.json` الخاص بالحزمة:

- Armor HUD — عرض الدرع
- Direction HUD — عرض الاتجاه
- Dynamic Crosshair — مؤشر تصويب متغير
- Killaura ESP *(Just a Vision)* — تأثير بصري فقط
- Original User Interfaces — واجهات مستخدم أصلية
- 3D Throwing Projectiles — مقذوفات ثلاثية الأبعاد
- 2D Fishing Hook — صنارة صيد ثنائية الأبعاد
- Cape Physics — فيزياء العباءة
- Armor Stand Like Player — درع عرض يشبه اللاعب
- First Person Hand Like Java Edition — يد المنظور الأول بنمط Java
- Fog — الضباب (عبر الحزم الفرعية)
- Music Player — مشغل موسيقى داخل اللعبة
- Lower Fire — إخفاء/تقليل النار
- Effect HUD — عرض التأثيرات
- Java Style Durability Bar — شريط متانة بنمط Java
- FPS Boost — تحسينات أداء بسيطة (إعدادات موارد)

### الحزم الفرعية | Subpacks

| المجلد | الاسم | الوصف |
| --- | --- | --- |
| `subpacks/foggy` | Foggy | ضباب مضبوط مخصص |
| `subpacks/nofog` | No Fog | إزالة الضباب |

### قائمة الإعدادات داخل اللعبة

`ui/!config.txt` يحتوي على إعدادات قائمة المود (Mod Menu) الافتراضية مثل
الأشرطة الجانبية، شريط الأدرينالين، عرض الدرع، مؤشر التصويب، نوع الخلفية، وغيرها.
يمكن تعديل هذا الملف لحفظ الإعدادات بشكل دائم بين الجلسات.

---

## هيكل المشروع | Project structure

```
.
├── manifest.json              # تعريف الحزمة (الاسم، الإصدار، UUIDs، الحزم الفرعية)
├── pack_icon.png              # أيقونة الحزمة
├── LICENSE.txt                # رخصة المؤلف الأصلية (Greninja)
├── blocks.json                # تعديل أشكال/خامات الزجاج
├── ui/                        # واجهات المستخدم (تُدمج مع واجهات اللعبة الأصلية)
├── XeroxClient/               # ملفات UI الخاصة بالمشروع (HUD، قوائم، أنيميشن)
├── textures/                  # جميع النسيج (blocks, entity, ui, gui, misc)
├── font/                      # خطوط مخصصة + minecraft/
├── models/                    # موديلات الكيانات
├── animations/                # أنيميشنات
├── animation_controllers/     # متحكمات الأنيميشن
├── render_controllers/        # متحكمات الريندر
├── attachables/               # عناصر قابلة للتركيب
├── entity/                    # تعريفات الكيانات
├── particles/                 # الجزيئات (معدلة + أصلية)
├── fogs/                      # تعريفات الضباب
├── cameras/                   # إعدادات الكاميرا
├── materials/                 # ملفات المواد/الشيدر
├── sounds/                    # أصوات واجهة المستخدم
├── texts/                     # ملفات الترجمة (en_US, en_GB, zh_CN, zh_TW)
├── subpacks/                  # الحزم الفرعية (foggy / nofog)
├── scripts/                   # ملفات .ml وصفية (غير تنفيذية)
├── tools/package.py           # سكربت بناء .mcpack
├── docs/                      # وثائق إضافية
└── .github/workflows/build.yml# بناء تلقائي للحزمة عبر CI
```

شجرة الملفات الكاملة موجودة في [`docs/FILE_TREE.md`](docs/FILE_TREE.md).

---

## التثبيت | Installation

### الطريقة 1 — بناء الحزمة ثم تثبيتها

```bash
python3 tools/package.py
```
سينتج الملف `dist/XeroxClient-v1.0.2.mcpack`. افتح الملف على الجهاز
(أو انقله إلى الهاتف) وسيتم استيراده تلقائيًا في Minecraft.

### الطريقة 2 — تثبيت يدوي (Android)

1. اضغط المجلد كاملًا على شكل `.zip`.
2. غيّر الامتداد إلى `.mcpack`.
3. افتح الملف من مدير الملفات → سيُستورد إلى Minecraft.
4. من داخل اللعبة: **Settings → Global Resources → XeroxClient → Activate**.

### الطريقة 3 — Windows / Bedrock

1. انسخ محتوى المستودع إلى مجلد.
2. اضغط المجلد بصيغة `.zip` ثم غيّر الامتداد إلى `.mcpack`.
3. شغّل الملف لاستيراده في اللعبة.

> **ملاحظة:** الحزم الفرعية تظهر داخل إعدادات الحزمة في اللعبة، اختر `Foggy` أو `No Fog`.

---

## البناء | Build

```bash
# ملف .mcpack
python3 tools/package.py

# ملف .zip
python3 tools/package.py --zip

# مسار مخصص
python3 tools/package.py --out build/XeroxClient.mcpack
```

السكربت:
- يقرأ الإصدار من `manifest.json` تلقائيًا.
- يستثني ملفات المستودع (`README`, `tools`, `docs`, `.github`, `dist` ...).
- ينتج أرشيفًا نظيفًا يحتوي جذر الحزمة مباشرة (كما يتطلب Minecraft).

أيضًا يوجد Workflow في `.github/workflows/build.yml` يبني الحزمة عند كل Push
وعند إنشاء Tag، ويرفع الملف الناتج كـ Artifact.

---

## التعديل | Customizing

| ماذا تريد تعديله | أين |
| --- | --- |
| الإعدادات الافتراضية للمود منيو | `ui/!config.txt` |
| النصوص والترجمة | `texts/*.lang` |
| ألوان / صور الواجهة | `textures/ui/` و `textures/Greninja/` |
| عناصر الـ HUD | `XeroxClient/hud_screen/` |
| الضباب | `subpacks/foggy/biomes_client.json` و `fogs/` |
| الأصوات | `sounds/ui_sounds/` |
| الأيقونة | `pack_icon.png` |

> بعد أي تعديل، شغّل `python3 tools/package.py` من جديد للحصول على حزمة محدثة.

---

## الحقوق والرخصة | Credits & License

- **صانع الحزمة الأصلي:** Greninja — Bilibili: [Greninja_](https://space.bilibili.com/)
- **شكر خاص:** Vargoo، LiuXingGuanZhu، MapleLeaf *(كما هو مذكور داخل الحزمة)*

نص رخصة المؤلف موجود كاملًا في [`LICENSE.txt`](LICENSE.txt) ومُلخّصه:

> يمكن استخدام وتعديل الحزمة للاستخدام الشخصي فقط.
> يُمنع إعادة نشر الحزمة أو نسخة معدّلة منها، أو دمجها في حزم أخرى،
> أو استخدامها تجاريًا، أو إعادة رفعها دون إذن صريح من المؤلف.

هذا المستودع هو **نسخة مصدرية (unpack) للملف الموجود في Google Drive** بغرض
الحفظ والتعديل الشخصي والبناء، وليس إعادة نشر للحزمة. جميع الحقوق محفوظة لمؤلفها
**Greninja**. راجع [`NOTICE.md`](NOTICE.md) للتفاصيل.

---

## سجل الإصدارات | Changelog

| التاريخ | الحدث |
| --- | --- |
| 2026-10-29 | فك ضغط `XeroxClient - v1.0.2 Official.mcpack.zip` وإنشاء هذا المستودع بمصادقة الإصدار 1.0.2 |
