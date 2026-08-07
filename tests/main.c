/*
 * ami2ha -- host test runner
 *
 * Builds and runs on the development machine, not on the Amiga. It links
 * only src/core/, which is why that directory is kept free of OS calls.
 */
#include <stdio.h>

int tt_checks   = 0;
int tt_failures = 0;

void suite_buf(void);
void suite_json(void);
void suite_base64(void);
void suite_sha1(void);
void suite_ws(void);
void suite_entity(void);
void suite_http(void);
void suite_ha(void);
void suite_config(void);
void suite_charset(void);

int main(void)
{
    printf("ami2ha core tests\n\n");

    printf("buf\n");    suite_buf();
    printf("json\n");   suite_json();
    printf("base64\n"); suite_base64();
    printf("sha1\n");   suite_sha1();
    printf("ws\n");     suite_ws();
    printf("entity\n"); suite_entity();
    printf("http\n");   suite_http();
    printf("ha\n");     suite_ha();
    printf("config\n"); suite_config();
    printf("charset\n");suite_charset();

    printf("\n%d checks, %d failures\n", tt_checks, tt_failures);
    return tt_failures ? 1 : 0;
}
