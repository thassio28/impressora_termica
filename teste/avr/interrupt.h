#define ISR(v) void v(void)
void sim_tick(void);
#define sei() ((void)0)
#define cli() sim_tick()
