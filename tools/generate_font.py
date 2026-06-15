import os
import subprocess

# 1. 定义你项目真正用到的所有中文字符（小智也是这么做的，按需提取）
# 你可以从你的代码里提取所有 lv_label_set_text 中的中文，去重后放在这里
CHINESE_CHARS = "你好世界温度湿度设置确认取消返回菜单小智AI语音助手"
ASCII_RANGE = "0x20-0x7F"

# 2. 定义要生成的字体尺寸和配置
fonts_to_generate = [
    {"name": "font_chinese_14", "size": 14, "bpp": 1}, # 1bpp 省空间，适合小字
    {"name": "font_chinese_16", "size": 16, "bpp": 4}, # 4bpp 抗锯齿，适合正文
]

# 3. 循环调用 lv_font_conv 生成 .c 文件
for f in fonts_to_generate:
    cmd = f"""
    lv_font_conv --size {f['size']} --bpp {f['bpp']} --format lvgl \
    --font /Users/xiaosage/esp/test_daima/docs/fonts/stheiti3.ttf \
    -r {ASCII_RANGE} --symbols "{CHINESE_CHARS}" \
    -o components/lcd_st7789/{f['name']}.c
    """
    os.system(cmd)
    print(f"Generated {f['name']}.c")