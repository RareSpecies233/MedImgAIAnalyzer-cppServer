// 在include外部模块时需小心，防止重复引入(crow中有引入一些模块)

#include <crow.h>//如果遇到神秘问题可以尝试不要引入全量crow头文件而是只引入需要的
#include <thread>
#include "info_store.h"
#include "info_api.h"

int main(int argc, char **argv)
{
    std::cout << "======================================" << std::endl;
    std::cout << "| 数据库软件运行后请勿手动修改数据库 |" << std::endl;
    std::cout << "|------------------------------------|" << std::endl;
    std::cout << "|            CPU推理版本             |" << std::endl;
    std::cout << "======================================" << std::endl;
    std::string onnx_path;
    int infer_threads = static_cast<int>(std::thread::hardware_concurrency());
    if (infer_threads <= 0) infer_threads = 1;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--onnx") {
            if (i + 1 >= argc) {
                std::cerr << "错误: --onnx 参数缺少路径" << std::endl;
                return 1;
            }
            onnx_path = argv[++i];
        } else if (key == "--infer-threads") {
            if (i + 1 >= argc) {
                std::cerr << "错误: --infer-threads 参数缺少数值" << std::endl;
                return 1;
            }
            infer_threads = std::stoi(argv[++i]);
            if (infer_threads <= 0) {
                std::cerr << "错误: --infer-threads 必须大于0" << std::endl;
                return 1;
            }
        } else if (key == "--help" || key == "-h") {
            std::cout << "用法: ./main [--onnx <model.onnx>] [--infer-threads <N>]" << std::endl;
            return 0;
        }
    }
    if (onnx_path.empty()) {
        std::cerr << "警告: 未选中onnx文件，无法使用推理功能！！！" << std::endl;
    }
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
    register_info_routes(app, store, onnx_path, infer_threads);

    CROW_ROUTE(app, "/api/health")([](){
        crow::json::wvalue res;
        res["status"] = "ok";
        return crow::response{res};
    });

    // 监听 18080 端口
    app.port(18080).multithreaded().run();
    return 0;
}
