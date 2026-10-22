#undef NDEBUG
#include <cassert>
#include <cstring>
#include "../../kernel/src/credential_ids.h"
#include "../../kernel/src/unix_credentials.h"
#include "../../kernel/src/desktop_identity.h"
using namespace brook;
static void expect(CredentialIds ids, uint32_t r, uint32_t e, uint32_t s) {
    assert(ids.real == r && ids.effective == e && ids.saved == s);
}
int main() {
    const brook::CredentialIds scmUsers{1000,1001,1002},scmGroups{2000,2001,2002};
    assert(brook::UnixCredentialsCheck({42,1000,2000},42,scmUsers,scmGroups)==0);
    assert(brook::UnixCredentialsCheck({42,1001,2002},42,scmUsers,scmGroups)==0);
    assert(brook::UnixCredentialsCheck({43,1000,2000},42,scmUsers,scmGroups)==-1);
    assert(brook::UnixCredentialsCheck({42,1003,2000},42,scmUsers,scmGroups)==-1);
    assert(brook::UnixCredentialsCheck({42,1000,2003},42,scmUsers,scmGroups)==-1);
    assert(brook::UnixCredentialsCheck({42,~uint32_t{0},2000},42,scmUsers,scmGroups)==-22);
    // Root IDs do not invent capability override support.
    assert(brook::UnixCredentialsCheck({42,1000,2000},42,{0,0,0},{0,0,0})==-1);

    uint32_t parsed = 123;
    assert(ParseLaunchId("0", &parsed) && parsed == 0);
    assert(ParseLaunchId("4294967294", &parsed) && parsed == 0xfffffffeU);
    const char* invalidIds[] = {"", "-1", "+1", "1000oops", "4294967295", "4294967296", "99999999999999999999"};
    for (const char* invalid : invalidIds)
        assert(!ParseLaunchId(invalid, &parsed));
    assert(LaunchPrimaryGroup(DesktopUid) == DesktopGid);
    assert(LaunchPrimaryGroup(0) == 0 && LaunchPrimaryGroup(1234) == 1234);
    char env[32];
    LaunchIdentityEnvironment(env, false, 0); assert(!strcmp(env, "USER=root"));
    LaunchIdentityEnvironment(env, true, DesktopUid); assert(!strcmp(env, "LOGNAME=brook"));
    LaunchIdentityEnvironment(env, false, 0xfffffffeU); assert(!strcmp(env, "USER=4294967294"));
    constexpr auto keep = CredentialUnchanged;
    auto ids = CredentialIds{1000, 0, 0};
    assert(!ChangeCredentialIds(ids, CredentialChange::Res, true, keep, 1000));
    expect(ids,1000,1000,0); // temporary effective drop preserves saved root
    assert(!ChangeCredentialIds(ids, CredentialChange::Res, false, keep, 0));
    expect(ids,1000,0,0);
    assert(!ChangeCredentialIds(ids, CredentialChange::Single, true, 1000));
    expect(ids,1000,1000,1000); // irreversible privileged setuid
    assert(ChangeCredentialIds(ids, CredentialChange::Single, false, 0) == -1);
    expect(ids,1000,1000,1000);
    assert(ChangeCredentialIds(ids, CredentialChange::Single, true, keep) == -22);
    expect(ids,1000,1000,1000);
    ids = {10,20,30};
    assert(!ChangeCredentialIds(ids, CredentialChange::Single, false,30));
    expect(ids,10,30,30); // unprivileged setuid changes effective only
    ids = {10,20,30};
    assert(!ChangeCredentialIds(ids, CredentialChange::Res, false,30,10,20));
    expect(ids,30,10,20); // every target checked against old triple
    assert(ChangeCredentialIds(ids, CredentialChange::Res, false,10,99,30) == -1);
    expect(ids,30,10,20); // denied transition cannot partially change real ID
    ids = {10,20,30};
    assert(!ChangeCredentialIds(ids, CredentialChange::Re, false,keep,10));
    expect(ids,10,10,30); // effective == old real does not change saved
    ids = {10,20,30};
    assert(!ChangeCredentialIds(ids, CredentialChange::Re, false,keep,30));
    expect(ids,10,30,30);
    ids = {10,20,30};
    assert(!ChangeCredentialIds(ids, CredentialChange::Re, false,20,10));
    expect(ids,20,10,10); // supplying real updates saved even when swapping
    ids = {10,20,30};
    assert(ChangeCredentialIds(ids, CredentialChange::Re, false,30,10) == -1);
    expect(ids,10,20,30); // saved is not an unprivileged setreuid real target
}
