// main.cpp - 最小 Crow 服务示例

#include <crow.h>//如果遇到神秘问题可以尝试不要引入全量crow头文件而是只引入需要的
#include "info_store.h"
#include "info_api.h"

int main()
{
    crow::SimpleApp app;

    // 初始化数据库（若缺失则创建 db/info.json）
    InfoStore store;
    try {
        store.init("db");
    } catch (const std::exception &e) {
        fprintf(stderr, "failed to initialize info store: %s\n", e.what());
        return 1;
    }

    // 注册项目信息接口（所有路径前缀为 /api/）
    register_info_routes(app, store);

    CROW_ROUTE(app, "/api/health")([](){
        crow::json::wvalue res;
        res["status"] = "ok";
        return crow::response{res};
    });

    // 监听 18080 端口
    app.port(18080).multithreaded().run();
    return 0;
}
