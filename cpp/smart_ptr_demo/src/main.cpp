#include <iostream>
#include <memory>
#include <string>

class Widget {
public:
    explicit Widget(std::string name) : name_(std::move(name)) {
        std::cout << "[Widget] 创建: " << name_ << "\n";
    }
    ~Widget() {
        std::cout << "[Widget] 销毁: " << name_ << "\n";
    }
    void use() const {
        std::cout << "[Widget] 使用: " << name_ << "\n";
    }
private:
    std::string name_;
};

int main() {
    std::cout << "=== Step 1: unique_ptr 基本用法 ===\n";
    {
        auto w = std::make_unique<Widget>("alpha");
        w->use();
        // 离开作用域，Widget 自动销毁，不需要 delete
    }

    std::cout << "\n=== Step 1.5: 独占性 ===\n";
    {
        auto w1 = std::make_unique<Widget>("beta");
        std::cout << "w1 是否为空? " << (w1 ? "否" : "是") << "\n";

        // 下面这行会编译失败，unique_ptr 不能拷贝：
        // auto w2 = w1;

        // 必须用 std::move 转移所有权：
        auto w2 = std::move(w1);
        std::cout << "move 之后，w1 是否为空? " << (w1 ? "否" : "是") << "\n";
        std::cout << "move 之后，w2 是否为空? " << (w2 ? "否" : "是") << "\n";

        w2->use();
        // w2 离开作用域，Widget 销毁
    }
    std::cout << "=== 全部结束 ===\n";
    return 0;
}
