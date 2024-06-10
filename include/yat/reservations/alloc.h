#ifndef YAT_RESERVATIONS_ALLOC_H
#define YAT_RESERVATIONS_ALLOC_H

#include <yat/reservations/reservation.h>

long alloc_polling_reservation(
	int res_type,
	struct reservation_config *config,
	struct reservation **_res);

long alloc_table_driven_reservation(
	struct reservation_config *config,
	struct reservation **_res);

#endif