#pragma once
namespace brook {
// Boot-only explicit diagnostic: opt/page-store-probe=<whole device name>.
// Destructively exercises slots only on a recognized disposable page-store
// image, before tasks/module unloading can race the device's lifetime.
void PageStoreBootProbe();
}
