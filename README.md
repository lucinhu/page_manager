# Page Manager

一个面向嵌入式 LVGL 的轻量级页面栈管理器，同时兼容 **ESP-IDF** 和 **标准 CMake**（PC 模拟器）。

## 特性

- 页面栈导航（push / pop / replace）
- 页面切换动画（none / slide / fade / custom）
- 页面生命周期回调（create / show / hide / destroy / event）
- 线程安全的事件投递（lock-free MPSC 队列）
- 导航数据传递（push 时携带数据，页面内读取）
- 页面结果回传（页面向外发送数据）
- keep_alive 模式（页面在后台保持存活）
- 平台抽象（ESP-IDF 使用 PSRAM 优先分配 + esp_log，PC 使用 malloc + printf）

## 集成方式

### ESP-IDF

将本目录放入 `components/` 下，CMakeLists.txt 自动走 `idf_component_register` 路径：

```
project/
├── components/
│   └── page_manager/      ← 放这里
│       ├── CMakeLists.txt
│       ├── include/
│       │   └── page_manager.h
│       └── src/
│           └── page_manager.c
├── main/
└── CMakeLists.txt
```

无需额外配置，ESP-IDF 构建系统自动发现组件。

### 标准 CMake

通过 `add_subdirectory` 引入，生成 `page_manager` 静态库目标：

```cmake
# 你的 CMakeLists.txt
add_subdirectory(path/to/page_manager page_manager)

add_executable(my_app main.c)
target_link_libraries(my_app page_manager lvgl)
```

前提：消费者必须提供名为 `lvgl` 的 CMake 目标（可以是 `add_library` 或 `find_package` 产生的）。

## API 参考

### 管理器生命周期

| API | 说明 |
|---|---|
| `pm_create(disp)` | 创建页面管理器，绑定到指定显示（NULL 用默认显示） |
| `pm_destroy(pm)` | 销毁管理器，释放所有资源 |

### 页面注册与导航

| API | 说明 |
|---|---|
| `pm_register_page(pm, name, callbacks, user_data)` | 注册页面类型（不会压栈） |
| `pm_push(pm, page)` | 压入页面 |
| `pm_push_with_data(pm, page, data, len)` | 压入页面并携带导航数据 |
| `pm_pop(pm)` | 弹出栈顶页面（不能弹出最后一个） |
| `pm_pop_to(pm, page)` | 弹出到指定页面 |
| `pm_replace_top(pm, page)` | 替换栈顶页面 |
| `pm_replace_top_with_data(pm, page, data, len)` | 替换栈顶并携带导航数据 |
| `pm_switch_to(pm, page)` | 切换主页面，不表达返回历史 |
| `pm_switch_to_with_data(pm, page, data, len)` | 切换主页面并携带导航数据 |
| `pm_create_page_instance(pm, template, data, len)` | 从注册页模板创建独立页面实例 |
| `pm_push_instance(pm, template, data, len)` | 创建并压入一个独立页面实例 |
| `pm_destroy_page_instance(pm, page)` | 销毁未入栈/未显示的页面实例 |
| `pm_show_overlay(pm, page)` | 在当前页面之上显示悬浮页 |
| `pm_show_overlay_with_data(pm, page, data, len)` | 显示悬浮页并携带导航数据 |
| `pm_close_overlay(pm)` | 关闭最上层悬浮页 |
| `pm_close_all_overlays(pm)` | 关闭所有悬浮页 |

### 页面查询

| API | 说明 |
|---|---|
| `pm_get_top_page(pm)` | 获取栈顶页面 |
| `pm_get_page_at(pm, index)` | 按索引获取页面（0 = 栈底） |
| `pm_get_stack_depth(pm)` | 获取栈深度 |
| `pm_get_page_root(page)` | 获取页面的 LVGL 根对象 |
| `pm_get_page_name(page)` | 获取页面名称 |
| `pm_get_page_user_data(page)` | 获取注册时的 user_data |
| `pm_has_page(pm, page)` | 页面是否在栈中 |
| `pm_is_top(pm, page)` | 页面是否是栈顶 |
| `pm_get_nav_data(page, &out_len)` | 获取页面的导航数据 |
| `pm_get_top_overlay(pm)` | 获取最上层悬浮页 |
| `pm_get_overlay_depth(pm)` | 获取悬浮页层数 |

### 页面配置

| API | 说明 |
|---|---|
| `pm_page_set_keep_alive(page, enable)` | 设置 keep_alive（被覆盖时保留而不销毁） |
| `pm_page_get_keep_alive(page)` | 查询 keep_alive 状态 |

### 页面切换动画

| API | 说明 |
|---|---|
| `pm_set_transition_style(pm, style)` | 设置切换动画风格 |
| `pm_get_transition_style(pm)` | 获取当前动画风格 |
| `pm_set_transition_duration(pm, ms)` | 设置动画时长，0 表示关闭动画 |
| `pm_get_transition_duration(pm)` | 获取动画时长 |
| `pm_set_custom_transition(pm, start_cb, cancel_cb, user_data)` | 注册自定义动画回调 |

内置风格：

| 风格 | 说明 |
|---|---|
| `PM_TRANSITION_STYLE_NONE` | 默认不播放动画，立即完成切换 |
| `PM_TRANSITION_STYLE_SLIDE` | 横向滑动动画 |
| `PM_TRANSITION_STYLE_FADE` | 新旧页面淡入淡出 |
| `PM_TRANSITION_STYLE_CUSTOM` | 使用 `pm_set_custom_transition()` 注册的回调 |

### 事件系统（外部 → 页面）

| API | 说明 |
|---|---|
| `pm_send_event(pm, event_id, data, len)` | 线程安全投递事件到栈顶页面 |
| `pm_broadcast_event(pm, event_id, data, len)` | 线程安全广播事件到栈内所有页面 |
| `pm_dispatch_events(pm)` | 立即处理已排队事件（LVGL 线程/锁内调用） |
| `pm_clear_events(pm)` | 丢弃尚未处理的排队事件 |

### 结果回传（页面 → 外部）

| API | 说明 |
|---|---|
| `pm_on_action(pm, handler, user_data)` | 注册全局结果回调 |
| `pm_send_action(pm, result_id, data, len)` | 页面向外发送结果数据 |

## 页面生命周期回调

```c
typedef struct {
    void (*on_create)(lv_obj_t *root, void *user_data);
    void (*on_show)(lv_obj_t *root, void *user_data);
    void (*on_hide)(lv_obj_t *root, void *user_data);
    void (*on_destroy)(lv_obj_t *root, void *user_data);
    void (*on_event)(lv_obj_t *root, int event_id,
                     const void *data, size_t data_len, void *user_data);
} pm_page_cbs_t;
```

| 回调 | 触发时机 | 典型用途 |
|---|---|---|
| `on_create` | 页面首次进入栈 | 创建 UI 控件、绑定事件 |
| `on_show` | 页面变为可见 | 刷新数据、启动动画 |
| `on_hide` | 页面被新页面覆盖（keep_alive 时） | 暂停动画 |
| `on_destroy` | 页面从栈中移除 | 释放资源 |
| `on_event` | 收到外部事件 | 更新 UI 数据 |

所有回调在 LVGL 线程执行，持有 LVGL 锁。

## 数据流

```
         pm_send_event()              on_event 回调
外部 ───────────────────→ 页面管理器 ──────────────→ 页面

         pm_push_with_data()          pm_get_nav_data()
外部 ───────────────────→ 导航数据 ──────────────→ 页面

         pm_send_action()             action_handler 回调
页面 ───────────────────→ 页面管理器 ──────────────→ 外部
```

## 使用示例

### 注册页面并导航

```c
// 定义页面回调
static void my_page_on_create(lv_obj_t *root, void *user_data) {
    lv_obj_t *label = lv_label_create(root);
    lv_label_set_text(label, "Hello");
}

static void my_page_on_event(lv_obj_t *root, int event_id,
                              const void *data, size_t len, void *user_data) {
    if (event_id == EVT_UPDATE_TEXT) {
        // 处理事件
    }
}

// 注册并使用
pm_page_cbs_t cbs = {
    .on_create = my_page_on_create,
    .on_event  = my_page_on_event,
};
pm_page_t *page = pm_register_page(pm, "MyPage", &cbs, NULL);
pm_push(pm, page);
```

### 线程安全地发送事件

```c
// 从任意线程调用（FreeRTOS 任务、定时器回调等）
// 不需要持有 LVGL 锁
void timer_callback(void) {
    app_time_event_t evt = {.time = "12:00"};
    pm_send_event(g_pm, EVT_TIME_UPDATE, &evt, sizeof(evt));
}
```

### 页面携带导航数据

```c
// 压入时携带数据
wifi_info_t info = {.ssid = "MyWiFi"};
pm_push_with_data(pm, wifi_page, &info, sizeof(info));

// 页面内读取
static void wifi_on_create(lv_obj_t *root, void *user_data) {
    size_t len;
    const wifi_info_t *info = pm_get_nav_data(
        /* 需要 page handle，通常通过 user_data 传入 */, &len);
}
```

### 页面回传结果

```c
// 外部注册结果处理器
static void on_page_result(pm_page_t *page, int result_id,
                            const void *data, size_t len, void *user_data) {
    if (result_id == RESULT_WIFI_SELECTED) {
        const char *ssid = (const char *)data;
        printf("用户选择了 WiFi: %s\n", ssid);
    }
}
pm_on_action(pm, on_page_result, NULL);

// 页面内部（比如按钮点击回调中）发出结果
// pm 句柄通过 user_data 或全局变量获得
static void on_btn_click(lv_event_t *e) {
    pm_t *pm = /* ... */;
    pm_send_action(pm, RESULT_WIFI_SELECTED, "MyWiFi", strlen("MyWiFi") + 1);
}
```

### keep_alive 页面

```c
// 注册后设置，页面被覆盖时不会销毁，回来时不会重新创建
pm_page_t *chat = pm_register_page(pm, "Chat", &chat_cbs, NULL);
pm_page_set_keep_alive(chat, true);
```

### 主页面切换

```c
// 适合 tab / 首页几个主功能之间切换
pm_page_t *chat = pm_register_page(pm, "Chat", &chat_cbs, NULL);
pm_page_t *camera = pm_register_page(pm, "Camera", &camera_cbs, NULL);

pm_page_set_keep_alive(chat, true);
pm_page_set_keep_alive(camera, true);

pm_switch_to(pm, chat);
pm_switch_to(pm, camera);
```

`pm_switch_to()` 不表示“进入下一页”，而是“当前主页面换成另一个”。它会关闭 overlay，隐藏旧主页面，显示目标页面；设置了 `keep_alive` 的主页面会保留状态。

### 悬浮页面

```c
pm_page_t *dialog = pm_register_page(pm, "Dialog", &dialog_cbs, NULL);

pm_show_overlay(pm, dialog);
pm_close_overlay(pm);
```

overlay 显示时，下面的页面仍然 visible，不触发 `on_hide()`。普通 `pm_send_event()` 会优先发给最上层 overlay；`pm_broadcast_event()` 会发给页面栈和 overlay 栈。

### 同类型多实例页面

```c
pm_page_t *detail = pm_register_page(pm, "Detail", &detail_cbs, NULL);

detail_nav_t a = {.id = 1};
detail_nav_t b = {.id = 2};

pm_push_instance(pm, detail, &a, sizeof(a));
pm_push_instance(pm, detail, &b, sizeof(b));
```

注册页默认是单例；如果要连续打开多个同类型但内容不同的页面，用 `pm_push_instance()`。实例页面会复制模板页的回调和 user_data，并拥有独立的 `nav_data/root/生命周期状态`，从栈或 overlay 移除时自动释放。

### 自定义切换动画

```c
static void anim_x_cb(void *var, int32_t x) {
    lv_obj_set_x((lv_obj_t *)var, x);
}

typedef struct {
    pm_transition_done_cb_t done_cb;
    void *done_user_data;
} my_transition_done_t;

static void custom_done_ready_cb(lv_anim_t *a) {
    my_transition_done_t *done =
        (my_transition_done_t *)lv_anim_get_user_data(a);
    if (done && done->done_cb) {
        done->done_cb(done->done_user_data);
    }
}

static my_transition_done_t s_transition_done;

static void my_transition(const pm_transition_ctx_t *ctx) {
    s_transition_done.done_cb = ctx->done_cb;
    s_transition_done.done_user_data = ctx->done_user_data;

    lv_obj_set_x(ctx->new_root, ctx->width);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ctx->new_root);
    lv_anim_set_exec_cb(&a, anim_x_cb);
    lv_anim_set_values(&a, ctx->width, 0);
    lv_anim_set_duration(&a, ctx->duration_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_user_data(&a, &s_transition_done);
    lv_anim_set_ready_cb(&a, custom_done_ready_cb);
    lv_anim_start(&a);
}

static void my_transition_cancel(const pm_transition_ctx_t *ctx) {
    if (ctx->new_root) {
        lv_anim_delete(ctx->new_root, anim_x_cb);
    }
}

pm_set_custom_transition(pm, my_transition, my_transition_cancel, NULL);
pm_set_transition_style(pm, PM_TRANSITION_STYLE_CUSTOM);
```

自定义动画的生命周期规则：

- `ctx` 只在 `my_transition()` 调用期间有效；异步动画需要保存 `old_root/new_root/duration_ms/done_cb/done_user_data` 等值。
- 页面栈变更、旧页面 hide/destroy、新页面归位仍由 page_manager 处理。
- 自定义动画结束时必须调用 `ctx->done_cb(ctx->done_user_data)` 一次，否则页面切换会一直处于进行中。
- 如果切换过程中又发起新导航，page_manager 会先调用自定义 `cancel_cb`，结束当前切换，再开始新的切换。

## 编译配置

通过编译宏可调整行为（在 CMake 中通过 `target_compile_definitions` 或 ESP-IDF 的 `menuconfig` 设置）：

| 宏 | 默认值 | 说明 |
|---|---|---|
| `PM_MAX_STACK_DEPTH` | 16 | 最大页面栈深度 |
| `PM_MAX_OVERLAY_DEPTH` | 4 | 最大悬浮页层数 |
| `PM_ANIM_DURATION_MS` | 180 | 页面切换动画时长（0 = 无动画） |
| `PM_ANIM_PATH_CB` | `lv_anim_path_ease_out` | 页面切换动画路径函数 |
| `PM_EVENT_POLL_MS` | 10 | 事件队列轮询间隔 |
| `PM_EVENT_DISPATCH_LIMIT` | 0 | 单次处理事件上限（0 = 不限） |

## 目录结构

```
page_manager/
├── CMakeLists.txt        # 双模式构建（ESP-IDF / 标准 CMake）
├── README.md
├── include/
│   └── page_manager.h    # 公共 API 头文件
└── src/
    └── page_manager.c    # 实现（含平台抽象层）
```
