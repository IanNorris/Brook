/* Host-only test adapter. Production main is intentionally fixed to the
 * kernel-admitted /media/system mount and maintenance context. */
#define BROOK_SSD_SLOT_TEST
#define main hardware_ssd_slots_production_main
#include "../../../tools/hardware_ssd_slots.c"
#undef main
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"--policy-selftest")) {
        const char *accepted[]={"TARGET=x\n", "UPDATE_SLOTS=0", "UPDATE_SLOTS=0\r\n", "TARGET=x\nUPDATE_SLOTS=0\n"};
        const char *rejected[]={"UPDATE_SLOTS=1", "UPDATE_SLOTS=0\nUPDATE_SLOTS=0", " UPDATE_SLOTS=0", "UPDATE_SLOTS=00"};
        for(size_t i=0;i<sizeof(accepted)/sizeof(accepted[0]);++i)
            if(!config_allows_activation((const uint8_t*)accepted[i],strlen(accepted[i])))return 4;
        for(size_t i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i)
            if(config_allows_activation((const uint8_t*)rejected[i],strlen(rejected[i])))return 5;
        return 0;
    }
    uint8_t expected[32];
    if(argc!=3 || hash_arg(argv[2],expected))return 2;
    int root=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0)return 3;
    int result=setup(root,expected);close(root);return result;
}
