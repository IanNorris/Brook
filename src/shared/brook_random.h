#pragma once

/* Brook getrandom extension. A request with this flag never uses the
 * compatibility software generator. EAGAIN means hardware entropy failed. */
#define BROOK_GRND_STRONG_ONLY (1u << 30)
