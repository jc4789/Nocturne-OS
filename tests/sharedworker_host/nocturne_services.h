/* ホストの試験の代役：サービスの口。 */
#pragma once
#include <stddef.h>
#include "../../common/services_abi.h"
int n_service_connect(const char *);
int n_connector_connect(int, const char *);
int n_connector_accept(int, char *, size_t);
