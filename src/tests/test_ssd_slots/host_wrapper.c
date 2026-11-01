/* Host-only test adapter. Production main is intentionally fixed to the
 * kernel-admitted /media/system mount and maintenance context. */
#define BROOK_SSD_SLOT_TEST
#define main hardware_ssd_slots_production_main
#include "../../../tools/hardware_ssd_slots.c"
#undef main
int main(int argc,char** argv) {
    uint8_t expected[32];
    if(argc!=3 || hash_arg(argv[2],expected))return 2;
    int root=open(argv[1],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(root<0)return 3;
    int result=setup(root,expected);close(root);return result;
}
