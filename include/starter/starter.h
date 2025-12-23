class Storage;
/**
 * 启动器
 */
class EyaKVStarter
{
private:
    static Storage storage;
    EyaKVStarter() = default;
    static void print_banner();

    static void initialize();

    static void initialize_logger();

    static Storage &initialize_storage();

    static void initialize_server();

    static void register_signal_handlers();

public:
    /**
     * 启动
     */
    static void start();
};