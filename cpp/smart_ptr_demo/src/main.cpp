#include <iostream>
#include <memory>
#include <string>
#include <cstdio>
#include <vector>

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
    const std::string& name() const { return name_; }
private:
    std::string name_;
};

class Shape {
public:
    virtual ~Shape() = default;
    virtual void draw() const = 0;
};

class Circle : public Shape {
public:
    void draw() const override { std::cout << "绘制 Circle\n"; }
};

class Square : public Shape {
public:
    void draw() const override { std::cout << "绘制 Square\n"; }
};

std::unique_ptr<Widget> make_widget(std::string name) {
    return std::make_unique<Widget>(std::move(name));
}

void observe(const Widget& w) {
    std::cout << "[observe] 观察到: " << w.name() << "\n";
}

void take_ownership(std::unique_ptr<Widget> w) {
    std::cout << "[take] 接管了: " << w->name() << "\n";
    w->use();
}

struct FileCloser {
    void operator()(std::FILE* fp) const {
        if (fp) {
            std::fclose(fp);
            std::cout << "[FileCloser] 文件已关闭\n";
        }
    }
};

int main() {
    std::cout << "=== 1. RAII: 离开作用域自动释放 ===\n";
    {
        auto w = std::make_unique<Widget>("alpha");
        w->use();
    }

    std::cout << "\n=== 2. 独占: 不能拷贝 ===\n";
    {
        auto w1 = std::make_unique<Widget>("beta");
        // auto w2 = w1; // 编译错误: use of deleted function
        std::cout << "w1 非空: " << (w1 ? "是" : "否") << "\n";
    }

    std::cout << "\n=== 3. move: 转移所有权 ===\n";
    {
        auto w1 = std::make_unique<Widget>("gamma");
        auto w2 = std::move(w1);
        std::cout << "move 后 w1 非空: " << (w1 ? "是" : "否") << "\n";
        std::cout << "move 后 w2 非空: " << (w2 ? "是" : "否") << "\n";
        w2->use();
    }

    std::cout << "\n=== 4. get / release / reset ===\n";
    {
        auto w = std::make_unique<Widget>("delta");
        Widget* raw = w.get();
        std::cout << "get 得到裸指针: " << raw->name() << "\n";
        std::cout << "get 后 w 非空: " << (w ? "是" : "否") << "\n";

        Widget* released = w.release();
        std::cout << "release 后 w 非空: " << (w ? "是" : "否") << "\n";
        std::cout << "release 得到: " << released->name() << "\n";
        delete released; // release 之后必须手动 delete

        auto w2 = std::make_unique<Widget>("epsilon");
        w2.reset();
        std::cout << "reset 后 w2 非空: " << (w2 ? "是" : "否") << "\n";
    }

    std::cout << "\n=== 5. 传参: observe vs take_ownership ===\n";
    {
        auto w = std::make_unique<Widget>("zeta");
        observe(*w);
        take_ownership(std::move(w));
        std::cout << "take 返回后 w 非空: " << (w ? "是" : "否") << "\n";
    }

    std::cout << "\n=== 6. 工厂函数 ===\n";
    {
        auto w = make_widget("eta");
        w->use();
    }

    std::cout << "\n=== 7. 容器里放多态对象 ===\n";
    {
        std::vector<std::unique_ptr<Shape>> shapes;
        shapes.push_back(std::make_unique<Circle>());
        shapes.push_back(std::make_unique<Square>());
        for (const auto& s : shapes) s->draw();
    }

    std::cout << "\n=== 8. 自定义删除器: 管理 FILE* ===\n";
    {
        std::FILE* fp = std::fopen("CMakeLists.txt", "r");
        if (fp) {
            std::unique_ptr<std::FILE, FileCloser> file(fp);
            std::cout << "文件打开成功，即将离开作用域\n";
        } else {
            std::cout << "打开文件失败（请从 smart_ptr_demo 目录运行）\n";
        }
    }

    std::cout << "\n=== 9. unique_ptr<T[]> ===\n";
    {
        auto arr = std::make_unique<int[]>(5);
        for (int i = 0; i < 5; ++i) arr[i] = i * 10;
        for (int i = 0; i < 5; ++i) std::cout << arr[i] << " ";
        std::cout << "\n";
    }

    std::cout << "\n=== 全部结束 ===\n";
    return 0;
}