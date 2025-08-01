/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2018 Gerhard Sittig <gerhard.sittig@gmx.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>
#include <math.h>
#include <string.h>
#include "protocol.h"

#define WITH_CMD_DELAY 0	/* TODO See which devices need delays. */

/* OWON XDM range definitions */
static const char *owon_dcv_ranges[] = {
	"auto", "50 mV", "500 mV", "5 V", "50 V", "500 V", "1000 V", NULL
};

static const char *owon_acv_ranges[] = {
	"auto", "500 mV", "5 V", "50 V", "500 V", "750 V", NULL
};

static const char *owon_dci_ranges[] = {
	"auto", "500 uA", "5 mA", "50 mA", "500 mA", "5 A", "10 A", NULL
};

static const char *owon_aci_ranges[] = {
	"auto", "500 uA", "5 mA", "50 mA", "500 mA", "5 A", "10 A", NULL
};

static const char *owon_res_ranges[] = {
	"auto", "500 Ohm", "5 kOhm", "50 kOhm", "500 kOhm", "5 MOhm", "50 MOhm", NULL
};

static const char *owon_cap_ranges[] = {
	"auto", "50 nF", "500 nF", "5 uF", "50 uF", "500 uF", "5 mF", "50 mF", NULL
};

static const char *owon_temp_ranges[] = {
	"KITS90", "Pt100", NULL /* no auto here */
};

SR_PRIV void scpi_dmm_cmd_delay(struct sr_scpi_dev_inst *scpi)
{
	if (WITH_CMD_DELAY)
		g_usleep(WITH_CMD_DELAY * 1000);

	if (!scpi->no_opc_command)
		sr_scpi_get_opc(scpi);
}

SR_PRIV const struct mqopt_item *scpi_dmm_lookup_mq_number(
	const struct sr_dev_inst *sdi, enum sr_mq mq, enum sr_mqflag flag)
{
	struct dev_context *devc;
	size_t i;
	const struct mqopt_item *item;

	devc = sdi->priv;
	for (i = 0; i < devc->model->mqopt_size; i++) {
		item = &devc->model->mqopts[i];
		if (item->mq != mq || item->mqflag != flag)
			continue;
		return item;
	}

	return NULL;
}

SR_PRIV const struct mqopt_item *scpi_dmm_lookup_mq_text(
	const struct sr_dev_inst *sdi, const char *text)
{
	struct dev_context *devc;
	size_t i;
	const struct mqopt_item *item;

	devc = sdi->priv;
	for (i = 0; i < devc->model->mqopt_size; i++) {
		item = &devc->model->mqopts[i];
		if (!item->scpi_func_query || !item->scpi_func_query[0])
			continue;
		if (!g_str_has_prefix(text, item->scpi_func_query))
			continue;
		return item;
	}

	return NULL;
}

SR_PRIV int scpi_dmm_get_mq(const struct sr_dev_inst *sdi,
	enum sr_mq *mq, enum sr_mqflag *flag, char **rsp,
	const struct mqopt_item **mqitem)
{
	struct dev_context *devc;
	const char *command;
	char *response;
	const char *have;
	int ret;
	const struct mqopt_item *item;

	devc = sdi->priv;
	if (mq)
		*mq = 0;
	if (flag)
		*flag = 0;
	if (rsp)
		*rsp = NULL;
	if (mqitem)
		*mqitem = NULL;

	scpi_dmm_cmd_delay(sdi->conn);
	command = sr_scpi_cmd_get(devc->cmdset, DMM_CMD_QUERY_FUNC);
	if (!command || !*command)
		return SR_ERR_NA;
	response = NULL;
	ret = sr_scpi_get_string(sdi->conn, command, &response);
	if (ret != SR_OK)
		return ret;
	if (!response || !*response) {
		g_free(response);
		return SR_ERR_NA;
	}
	have = response;
	if (*have == '"')
		have++;

	ret = SR_ERR_NA;
	item = scpi_dmm_lookup_mq_text(sdi, have);
	if (item) {
		if (mq)
			*mq = item->mq;
		if (flag)
			*flag = item->mqflag;
		if (mqitem)
			*mqitem = item;
		ret = SR_OK;
	} else {
		sr_warn("Unknown measurement quantity: %s", have);
	}

	if (rsp) {
		*rsp = response;
		response = NULL;
	}
	g_free(response);

	return ret;
}

SR_PRIV int scpi_dmm_set_mq(const struct sr_dev_inst *sdi,
	enum sr_mq mq, enum sr_mqflag flag)
{
	struct dev_context *devc;
	const struct mqopt_item *item;
	const char *mode, *command;
	int ret;

	devc = sdi->priv;
	item = scpi_dmm_lookup_mq_number(sdi, mq, flag);
	if (!item)
		return SR_ERR_NA;

	mode = item->scpi_func_setup;
	command = sr_scpi_cmd_get(devc->cmdset, DMM_CMD_SETUP_FUNC);
	scpi_dmm_cmd_delay(sdi->conn);
	ret = sr_scpi_send(sdi->conn, command, mode);
	if (ret != SR_OK)
		return ret;
	if (item->drv_flags & FLAG_CONF_DELAY)
		g_usleep(devc->model->conf_delay_us);

	return SR_OK;
}

SR_PRIV const char *scpi_dmm_get_range_text(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int ret;
	const struct mqopt_item *mqitem;
	gboolean is_auto;
	char *response, *pos;
	double range;
	int digits;

	devc = sdi->priv;

	ret = scpi_dmm_get_mq(sdi, NULL, NULL, NULL, &mqitem);
	if (ret != SR_OK)
		return NULL;
	if (!mqitem || !mqitem->scpi_func_setup)
		return NULL;
	if (mqitem->drv_flags & FLAG_NO_RANGE)
		return NULL;

	scpi_dmm_cmd_delay(sdi->conn);
	ret = sr_scpi_cmd(sdi, devc->cmdset, 0, NULL,
		DMM_CMD_QUERY_RANGE_AUTO, mqitem->scpi_func_setup);
	if (ret != SR_OK)
		return NULL;
	ret = sr_scpi_get_bool(sdi->conn, NULL, &is_auto);
	if (ret != SR_OK)
		return NULL;
	if (is_auto)
		return "auto";

	/*
	 * Get the response into a text buffer. The range value may be
	 * followed by a precision value separated by comma. Common text
	 * to number conversion support code may assume that the input
	 * text spans to the end of the text, need not accept trailing
	 * text which is not part of a number.
	 */
	scpi_dmm_cmd_delay(sdi->conn);
	ret = sr_scpi_cmd(sdi, devc->cmdset, 0, NULL,
		DMM_CMD_QUERY_RANGE, mqitem->scpi_func_setup);
	if (ret != SR_OK)
		return NULL;
	response = NULL;
	ret = sr_scpi_get_string(sdi->conn, NULL, &response);
	if (ret != SR_OK) {
		g_free(response);
		return NULL;
	}
	pos = strchr(response, ',');
	if (pos)
		*pos = '\0';
	ret = sr_atod_ascii_digits(response, &range, &digits);
	g_free(response);
	if (ret != SR_OK)
		return NULL;
	snprintf(devc->range_text, sizeof(devc->range_text), "%lf", range);
	return devc->range_text;
}

/*
 * We use human-readable range texts, including the unit. They are mostly the same
 * as displayed on the device, but with some differences:
 * - The unit is always separated from the number by a space.
 * - The Unicode Omega symbol (0xCE 0xA9) is replaced with "Ohm".
 *
 * Here is all the possible range answers, that I got from XDM1041 over SCPI:
 * DCV: 1000 V␍␊ 500 V␍␊ 50 V␍␊ 5 V␍␊ 500 mV␍␊ 50 mV␍␊
 * ACV 750 V␍␊500 V␍␊ 50 V␍␊5 V␍␊500 mV␍␊
 * DCI: 10 A␍␊5 A␍␊ 500 mA␍␊50 mA␍␊5 mA␍␊500 uA␍␊
 * ACI: 10 A␍␊5 A␍␊500 mA␍␊50 mA␍␊5 mA␍␊500 uA␍␊
 * RES: 50 M<0xce><0xa9>␍␊5 M<0xce><0xa9>␍␊500 K<0xce><0xa9>␍␊50 K<0xce><0xa9>␍␊5 K<0xce><0xa9>␍␊500 <0xce><0xa9>␍␊500 <0xce><0xa9>␍␊500 <0xce><0xa9>␍␊
 * CAP: 50 mF␍␊5 mF␍␊500uF␍␊50uF␍␊5uF␍␊500 nF␍␊50 nF␍␊
 * Freq: Hz␍␊
 * Period: s␍␊
 * Temp: KITS90␍␊Pt100␍␊
 */
SR_PRIV const char *scpi_dmm_owon_get_range_text(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int ret;
	const struct mqopt_item *mqitem;
	gboolean is_auto;
	char *response, *pos;

	devc = sdi->priv;

	ret = scpi_dmm_get_mq(sdi, NULL, NULL, NULL, &mqitem);
	if (ret != SR_OK)
		return NULL;
	if (!mqitem || !mqitem->scpi_func_setup)
		return NULL;
	if (mqitem->drv_flags & FLAG_NO_RANGE)
		return NULL;

	scpi_dmm_cmd_delay(sdi->conn);
	ret = sr_scpi_cmd(sdi, devc->cmdset, 0, NULL,
		DMM_CMD_QUERY_RANGE_AUTO, mqitem->scpi_func_setup);
	if (ret != SR_OK)
		return NULL;
	ret = sr_scpi_get_bool(sdi->conn, NULL, &is_auto);
	if (ret != SR_OK)
		return NULL;
	if (is_auto)
		return "auto";

	scpi_dmm_cmd_delay(sdi->conn);
	ret = sr_scpi_cmd(sdi, devc->cmdset, 0, NULL,
		DMM_CMD_QUERY_RANGE, mqitem->scpi_func_setup);
	if (ret != SR_OK)
		return NULL;
	response = NULL;
	ret = sr_scpi_get_string(sdi->conn, NULL, &response);
	if (ret != SR_OK) {
		g_free(response);
		return NULL;
	}
	/* Replace Unicode Omega symbol (0xCE 0xA9) with "Ohm". */
	GString *response_str = g_string_new(response);
	g_string_replace(response_str, "\xCE\xA9", "Ohm", 0);
	g_free(response);
	response = g_string_free(response_str, FALSE);

	/* Check if space is needed between number and units. */
	pos = response;

	/* Skip leading whitespace. */
	while (*pos && g_ascii_isspace(*pos))
		pos++;

	/* Find where the number ends. */
	char *number_end = pos;
	while (*number_end && (g_ascii_isdigit(*number_end) || *number_end == '.' ||
		*number_end == '+' || *number_end == '-' || *number_end == 'e' ||
		*number_end == 'E'))
		number_end++;

	/* Check if we found a number and there's a unit character immediately after (no space). */
	if (number_end > pos && *number_end && !g_ascii_isspace(*number_end)) {
		/* Need to insert space between number and units. */
		size_t number_len = number_end - pos;
		snprintf(devc->range_text, sizeof(devc->range_text), "%.*s %s", 
			(int)number_len, pos, number_end);
		g_free(response);
		return devc->range_text;
	} else {
		/* Response is fine as is, just copy it. */
		snprintf(devc->range_text, sizeof(devc->range_text), "%s", response);
		g_free(response);
		return devc->range_text;
	}
}

SR_PRIV int scpi_dmm_set_range_from_text(const struct sr_dev_inst *sdi,
	const char *range)
{
	struct dev_context *devc;
	int ret;
	const struct mqopt_item *item;
	gboolean is_auto;

	devc = sdi->priv;

	if (!range || !*range)
		return SR_ERR_ARG;

	ret = scpi_dmm_get_mq(sdi, NULL, NULL, NULL, &item);
	if (ret != SR_OK)
		return ret;
	if (!item || !item->scpi_func_setup)
		return SR_ERR_ARG;
	if (item->drv_flags & FLAG_NO_RANGE)
		return SR_ERR_NA;

	is_auto = g_ascii_strcasecmp(range, "auto") == 0;
	scpi_dmm_cmd_delay(sdi->conn);
	ret = sr_scpi_cmd(sdi, devc->cmdset, 0, NULL, DMM_CMD_SETUP_RANGE,
		item->scpi_func_setup, is_auto ? "AUTO" : range);
	if (ret != SR_OK)
		return ret;
	if (item->drv_flags & FLAG_CONF_DELAY)
		g_usleep(devc->model->conf_delay_us);

	return SR_OK;
}

/* OWON XDM DMMs have two different methods to set range:
 * "CONF:VOLT 0.05" will set the range to 50 mV (note the absence of units)
 * "RANGE 5" will set the range to fifth option in a list of possible ranges for current measurement mode
 * Although the second one would be easier to implement, the first one should not be affected by future changes in firmware
 */
SR_PRIV int scpi_dmm_owon_set_range_from_text(const struct sr_dev_inst *sdi,
										 const char *range)
{
	struct dev_context *devc;
	int ret;
	const struct mqopt_item *item;
	gboolean is_auto;
	char processed_range[64];

	devc = sdi->priv;

	if (!range || !*range)
		return SR_ERR_ARG;

	ret = scpi_dmm_get_mq(sdi, NULL, NULL, NULL, &item);
	if (ret != SR_OK)
		return ret;
	if (!item || !item->scpi_func_setup)
		return SR_ERR_ARG;
	if (item->drv_flags & FLAG_NO_RANGE)
		return SR_ERR_NA;

	is_auto = g_ascii_strcasecmp(range, "auto") == 0;

	/* Preprocess range text to handle SI prefixes */
	const char *space_pos = strchr(range, ' ');
	if (space_pos && *(space_pos + 1)) {
		/* Extract the numeric part */
		size_t num_len = space_pos - range;
		char num_str[32];
		strncpy(num_str, range, num_len);
		num_str[num_len] = '\0';

		/* Parse the numeric value */
		double value;
		ret = sr_atod_ascii(num_str, &value);
		if (ret == SR_OK) {
			/* Check for SI prefix after the space */
			char prefix = *(space_pos + 1);
			double multiplier = 1.0;

			switch (prefix) {
			case 'k':
			case 'K':
				multiplier = 1e3;
				break;
			case 'm':
				multiplier = 1e-3;
				break;
			case 'u':
				multiplier = 1e-6;
				break;
			case 'n':
				multiplier = 1e-9;
				break;
			case 'p':
				multiplier = 1e-12;
				break;
			case 'M':
				multiplier = 1e6;
				break;
			case 'G':
				multiplier = 1e9;
				break;
			default:
				/* No recognized SI prefix, use original range */
				snprintf(processed_range, sizeof(processed_range), "%s", range);
				multiplier = 0.0; /* Flag to use original range */
				break;
			}

			if (multiplier != 0.0) {
				/* Apply the multiplier and format the result with locale-independent formatting */
				value *= multiplier;
				g_ascii_dtostr(processed_range, sizeof(processed_range), value);
			}
		} else {
			/* Failed to parse number, use original range */
			snprintf(processed_range, sizeof(processed_range), "%s", range);
		}
	} else {
		/* No space found, use original range */
		snprintf(processed_range, sizeof(processed_range), "%s", range);
	}

	scpi_dmm_cmd_delay(sdi->conn);
	ret = sr_scpi_cmd(sdi, devc->cmdset, 0, NULL, DMM_CMD_SETUP_RANGE,
					  item->scpi_func_setup, is_auto ? "AUTO" : processed_range);
	if (ret != SR_OK)
		return ret;
	if (item->drv_flags & FLAG_CONF_DELAY)
		g_usleep(devc->model->conf_delay_us);

	return SR_OK;
}

SR_PRIV GVariant *scpi_dmm_get_range_text_list(const struct sr_dev_inst *sdi)
{
	GVariantBuilder gvb;
	GVariant *list;

	(void)sdi;

	g_variant_builder_init(&gvb, G_VARIANT_TYPE_ARRAY);
	/* TODO
	 * Add more items _when_ the connected device supports a fixed
	 * or known set of ranges. The Agilent protocol is flexible and
	 * tolerant, set requests accept any value, and the device will
	 * use an upper limit which is at least the specified value.
	 * The values are communicated as mere numbers without units.
	 */
	list = g_variant_builder_end(&gvb);

	return list;
}

SR_PRIV GVariant *scpi_dmm_owon_get_range_text_list(const struct sr_dev_inst *sdi)
{
	GVariantBuilder gvb;
	GVariant *list;
	int ret;
	enum sr_mq mq;
	enum sr_mqflag mqflag;
	const char **ranges = NULL;
	const struct mqopt_item *mqitem;
	int i;

	/* Explicitly use string array type, otherwise empty array won't be typed */
	g_variant_builder_init(&gvb, G_VARIANT_TYPE_STRING_ARRAY); 
	
	/* Get current measurement quantity to return appropriate ranges */
	ret = scpi_dmm_get_mq(sdi, &mq, &mqflag, NULL, &mqitem);
	if (ret != SR_OK) {
		/* Return empty list if we can't determine current mode */
		list = g_variant_builder_end(&gvb);
		return list;
	}

	/* Check if current mode has no range support */
	if (mqitem && (mqitem->drv_flags & FLAG_NO_RANGE)) {
		/* Return empty list for modes that don't support ranges */
		list = g_variant_builder_end(&gvb);
		return list;
	}
	/* Select appropriate range array based on current measurement type */
	switch (mq) {
	case SR_MQ_VOLTAGE:
		if (mqflag & SR_MQFLAG_DC) {
			ranges = owon_dcv_ranges;
		} else if (mqflag & SR_MQFLAG_AC) {
			ranges = owon_acv_ranges;
		}
		break;
	case SR_MQ_CURRENT:
		if (mqflag & SR_MQFLAG_DC) {
			ranges = owon_dci_ranges;
		} else if (mqflag & SR_MQFLAG_AC) {
			ranges = owon_aci_ranges;
		}
		break;
	case SR_MQ_RESISTANCE:
		ranges = owon_res_ranges;
		break;
	case SR_MQ_CAPACITANCE:
		ranges = owon_cap_ranges;
		break;
	case SR_MQ_TEMPERATURE:
		ranges = owon_temp_ranges;
		break;
	default:
		/* For other modes, just provide auto */
		break;
	}

	/* Add all ranges from the selected array */
	if (ranges) {
		for (i = 0; ranges[i] != NULL; i++) {
			g_variant_builder_add(&gvb, "s", ranges[i]);
		}
	}

	list = g_variant_builder_end(&gvb);
	return list;
}

SR_PRIV int scpi_dmm_get_meas_agilent(const struct sr_dev_inst *sdi, size_t ch)
{
	struct sr_scpi_dev_inst *scpi;
	struct dev_context *devc;
	struct scpi_dmm_acq_info *info;
	struct sr_datafeed_analog *analog;
	int ret;
	enum sr_mq mq;
	enum sr_mqflag mqflag;
	char *mode_response;
	const char *p;
	char **fields;
	size_t count;
	char prec_text[20];
	const struct mqopt_item *item;
	int prec_exp;
	const char *command;
	char *response;
	gboolean use_double;
	int sig_digits, val_exp;
	int digits;
	enum sr_unit unit;
	double limit;

	scpi = sdi->conn;
	devc = sdi->priv;
	info = &devc->run_acq_info;
	analog = &info->analog[ch];

	/*
	 * Get the meter's current mode, keep the response around.
	 * Skip the measurement if the mode is uncertain.
	 */
	ret = scpi_dmm_get_mq(sdi, &mq, &mqflag, &mode_response, &item);
	if (ret != SR_OK) {
		g_free(mode_response);
		return ret;
	}
	if (!mode_response)
		return SR_ERR;
	if (!mq) {
		g_free(mode_response);
		return +1;
	}

	/*
	 * Get the last comma separated field of the function query
	 * response, or fallback to the model's default precision for
	 * the current function. This copes with either of these cases:
	 *   VOLT +1.00000E-01,+1.00000E-06
	 *   DIOD
	 *   TEMP THER,5000,+1.00000E+00,+1.00000E-01
	 */
	p = sr_scpi_unquote_string(mode_response);
	fields = g_strsplit(p, ",", 0);
	count = g_strv_length(fields);
	if (count >= 2) {
		snprintf(prec_text, sizeof(prec_text),
			"%s", fields[count - 1]);
		p = prec_text;
	} else if (!item) {
		p = NULL;
	} else if (item->default_precision == NO_DFLT_PREC) {
		p = NULL;
	} else {
		snprintf(prec_text, sizeof(prec_text),
			"1e%d", item->default_precision);
		p = prec_text;
	}
	g_strfreev(fields);

	/*
	 * Need to extract the exponent value ourselves, since a strtod()
	 * call will "eat" the exponent, too. Strip space, strip sign,
	 * strip float number (without! exponent), check for exponent
	 * and get exponent value. Accept absence of Esnn suffixes.
	 */
	while (p && *p && g_ascii_isspace(*p))
		p++;
	if (p && *p && (*p == '+' || *p == '-'))
		p++;
	while (p && *p && g_ascii_isdigit(*p))
		p++;
	if (p && *p && *p == '.')
		p++;
	while (p && *p && g_ascii_isdigit(*p))
		p++;
	ret = SR_OK;
	if (!p || !*p)
		prec_exp = 0;
	else if (*p != 'e' && *p != 'E')
		ret = SR_ERR_DATA;
	else
		ret = sr_atoi(++p, &prec_exp);
	g_free(mode_response);
	if (ret != SR_OK)
		return ret;

	/*
	 * Get the measurement value. Make sure to strip trailing space
	 * or else number conversion may fail in fatal ways. Detect OL
	 * conditions. Determine the measurement's precision: Count the
	 * number of significant digits before the period, and get the
	 * exponent's value.
	 *
	 * The text presentation of values is like this:
	 *   +1.09450000E-01
	 * Skip space/sign, count digits before the period, skip to the
	 * exponent, get exponent value.
	 *
	 * TODO Can sr_parse_rational() return the exponent for us? In
	 * addition to providing a precise rational value instead of a
	 * float that's an approximation of the received value? Can the
	 * 'analog' struct that we fill in carry rationals?
	 *
	 * Use double precision FP here during conversion. Optionally
	 * downgrade to single precision later to reduce the amount of
	 * logged information.
	 */
	command = sr_scpi_cmd_get(devc->cmdset, DMM_CMD_QUERY_VALUE);
	if (!command || !*command)
		return SR_ERR_NA;
	scpi_dmm_cmd_delay(scpi);
	ret = sr_scpi_get_string(scpi, command, &response);
	if (ret != SR_OK)
		return ret;
	g_strstrip(response);
	use_double = devc->model->digits >= 6;
	ret = sr_atod_ascii(response, &info->d_value);
	if (ret != SR_OK) {
		g_free(response);
		return ret;
	}
	if (!response)
		return SR_ERR;
	limit = 9e37;
	if (info->d_value > +limit) {
		info->d_value = +INFINITY;
	} else if (info->d_value < -limit) {
		info->d_value = -INFINITY;
	} else {
		p = response;
		while (p && *p && g_ascii_isspace(*p))
			p++;
		if (p && *p && (*p == '-' || *p == '+'))
			p++;
		sig_digits = 0;
		while (p && *p && g_ascii_isdigit(*p)) {
			sig_digits++;
			p++;
		}
		if (p && *p && *p == '.')
			p++;
		while (p && *p && g_ascii_isdigit(*p))
			p++;
		ret = SR_OK;
		if (!p || !*p)
			val_exp = 0;
		else if (*p != 'e' && *p != 'E')
			ret = SR_ERR_DATA;
		else
			ret = sr_atoi(++p, &val_exp);
	}
	g_free(response);
	if (ret != SR_OK)
		return ret;
	/*
	 * TODO Come up with the most appropriate 'digits' calculation.
	 * This implementation assumes that either the device provides
	 * the resolution with the query for the meter's function, or
	 * the driver uses a fallback text pretending the device had
	 * provided it. This works with supported Agilent devices.
	 *
	 * An alternative may be to assume a given digits count which
	 * depends on the device, and adjust that count based on the
	 * value's significant digits and exponent. But this approach
	 * fails if devices change their digits count depending on
	 * modes or user requests, and also fails when e.g. devices
	 * with "100000 counts" can provide values between 100000 and
	 * 120000 in either 4 or 5 digits modes, depending on the most
	 * recent trend of the values. This less robust approach should
	 * only be taken if the mode inquiry won't yield the resolution
	 * (as e.g. DIOD does on 34405A, though we happen to know the
	 * fixed resolution for this very mode on this very model).
	 *
	 * For now, let's keep the prepared code path for the second
	 * approach in place, should some Agilent devices need it yet
	 * benefit from re-using most of the remaining acquisition
	 * routine.
	 */
#if 1
	digits = -prec_exp;
#else
	digits = devc->model->digits;
	digits -= sig_digits;
	digits -= val_exp;
#endif

	/*
	 * Fill in the 'analog' description: value, encoding, meaning.
	 * Callers will fill in the sample count, and channel name,
	 * and will send out the packet.
	 */
	if (use_double) {
		analog->data = &info->d_value;
		analog->encoding->unitsize = sizeof(info->d_value);
	} else {
		info->f_value = info->d_value;
		analog->data = &info->f_value;
		analog->encoding->unitsize = sizeof(info->f_value);
	}
	analog->encoding->digits = digits;
	analog->meaning->mq = mq;
	analog->meaning->mqflags = mqflag;
	switch (mq) {
	case SR_MQ_VOLTAGE:
		unit = SR_UNIT_VOLT;
		break;
	case SR_MQ_CURRENT:
		unit = SR_UNIT_AMPERE;
		break;
	case SR_MQ_RESISTANCE:
	case SR_MQ_CONTINUITY:
		unit = SR_UNIT_OHM;
		break;
	case SR_MQ_CAPACITANCE:
		unit = SR_UNIT_FARAD;
		break;
	case SR_MQ_TEMPERATURE:
		unit = SR_UNIT_CELSIUS;
		break;
	case SR_MQ_FREQUENCY:
		unit = SR_UNIT_HERTZ;
		break;
	case SR_MQ_TIME:
		unit = SR_UNIT_SECOND;
		break;
	default:
		return SR_ERR_NA;
	}
	analog->meaning->unit = unit;
	analog->spec->spec_digits = digits;

	return SR_OK;
}

SR_PRIV int scpi_dmm_get_meas_gwinstek(const struct sr_dev_inst *sdi, size_t ch)
{
	struct sr_scpi_dev_inst *scpi;
	struct dev_context *devc;
	struct scpi_dmm_acq_info *info;
	struct sr_datafeed_analog *analog;
	int ret;
	enum sr_mq mq;
	enum sr_mqflag mqflag;
	char *mode_response;
	const char *p;
	const struct mqopt_item *item;
	const char *command;
	char *response;
	gboolean use_double;
	double limit;
	int sig_digits, val_exp;
	int digits;
	enum sr_unit unit;
	int mmode;

	scpi = sdi->conn;
	devc = sdi->priv;
	info = &devc->run_acq_info;
	analog = &info->analog[ch];

	/*
	 * Get the meter's current mode, keep the response around.
	 * Skip the measurement if the mode is uncertain.
	 */
	ret = scpi_dmm_get_mq(sdi, &mq, &mqflag, &mode_response, &item);
	if (ret != SR_OK) {
		g_free(mode_response);
		return ret;
	}
	if (!mode_response)
		return SR_ERR;
	if (!mq) {
		g_free(mode_response);
		return +1;
	}
	mmode = atoi(mode_response);
	g_free(mode_response);

	/*
	 * Get the current reading from the meter.
	 */
	scpi_dmm_cmd_delay(scpi);
	command = sr_scpi_cmd_get(devc->cmdset, DMM_CMD_QUERY_VALUE);
	if (!command || !*command)
		return SR_ERR_NA;
	scpi_dmm_cmd_delay(scpi);
	ret = sr_scpi_get_string(scpi, command, &response);
	if (ret != SR_OK)
		return ret;
	g_strstrip(response);
	use_double = devc->model->digits > 6;
	ret = sr_atod_ascii(response, &info->d_value);
	if (ret != SR_OK) {
		g_free(response);
		return ret;
	}
	if (!response)
		return SR_ERR;
	limit = 9e37;
	if (devc->model->infinity_limit != 0.0)
		limit = devc->model->infinity_limit;
	if (info->d_value >= +limit) {
		info->d_value = +INFINITY;
	} else if (info->d_value <= -limit) {
		info->d_value = -INFINITY;
	} else {
		p = response;
		while (p && *p && g_ascii_isspace(*p))
			p++;
		if (p && *p && (*p == '-' || *p == '+'))
			p++;
		sig_digits = 0;
		while (p && *p && g_ascii_isdigit(*p)) {
			sig_digits++;
			p++;
		}
		if (p && *p && *p == '.')
			p++;
		while (p && *p && g_ascii_isdigit(*p))
			p++;
		ret = SR_OK;
		if (!p || !*p)
			val_exp = 0;
		else if (*p != 'e' && *p != 'E')
			ret = SR_ERR_DATA;
		else
			ret = sr_atoi(++p, &val_exp);
	}
	g_free(response);
	if (ret != SR_OK)
		return ret;

	/*
	 * Make sure we report "INFINITY" when meter displays "0L".
	 */
	switch (mmode) {
	case 7:
	case 16:
		/* In resitance modes 0L reads as 1.20000E8 or 1.99999E8. */
		limit = 1.2e8;
		if (strcmp(devc->model->model, "GDM8255A") == 0)
			limit = 1.99999e8;
		if (info->d_value >= limit)
			info->d_value = +INFINITY;
		break;
	case 13:
		/* In continuity mode 0L reads as 1.20000E3. */
		if (info->d_value >= 1.2e3)
			info->d_value = +INFINITY;
		break;
	case 17:
		/* In diode mode 0L reads as 1.00000E0. */
		if (info->d_value == 1.0e0)
			info->d_value = +INFINITY;
		break;
	}

	/*
	 * Calculate 'digits' based on the result of the optional
	 * precision reading which was done at acquisition start.
	 * The GW-Instek manual gives the following information
	 * regarding the resolution:
	 *
	 * Type      Digit
	 * --------  ------
	 * Slow      5 1/2
	 * Medium    4 1/2
	 * Fast      3 1/2
	 */
	digits = devc->model->digits;
	if (devc->precision && *devc->precision) {
		if (g_str_has_prefix(devc->precision, "Slow"))
			digits = 6;
		else if (g_str_has_prefix(devc->precision, "Mid"))
			digits = 5;
		else if (g_str_has_prefix(devc->precision, "Fast"))
			digits = 4;
		else
			sr_info("Unknown precision: '%s'", devc->precision);
	}

	/*
	 * Fill in the 'analog' description: value, encoding, meaning.
	 * Callers will fill in the sample count, and channel name,
	 * and will send out the packet.
	 */
	if (use_double) {
		analog->data = &info->d_value;
		analog->encoding->unitsize = sizeof(info->d_value);
	} else {
		info->f_value = info->d_value;
		analog->data = &info->f_value;
		analog->encoding->unitsize = sizeof(info->f_value);
	}
	analog->encoding->digits = digits;
	analog->meaning->mq = mq;
	analog->meaning->mqflags = mqflag;
	switch (mq) {
	case SR_MQ_VOLTAGE:
		unit = SR_UNIT_VOLT;
		break;
	case SR_MQ_CURRENT:
		unit = SR_UNIT_AMPERE;
		break;
	case SR_MQ_RESISTANCE:
	case SR_MQ_CONTINUITY:
		unit = SR_UNIT_OHM;
		break;
	case SR_MQ_CAPACITANCE:
		unit = SR_UNIT_FARAD;
		break;
	case SR_MQ_TEMPERATURE:
		switch (mmode) {
		case 15:
			unit = SR_UNIT_FAHRENHEIT;
			break;
		case 9:
		default:
			unit = SR_UNIT_CELSIUS;
		}
		break;
	case SR_MQ_FREQUENCY:
		unit = SR_UNIT_HERTZ;
		break;
	case SR_MQ_TIME:
		unit = SR_UNIT_SECOND;
		break;
	default:
		return SR_ERR_NA;
	}
	analog->meaning->unit = unit;
	analog->spec->spec_digits = digits;

	return SR_OK;
}

/* Strictly speaking this is a timer controlled poll routine. */
SR_PRIV int scpi_dmm_receive_data(int fd, int revents, void *cb_data)
{
	struct sr_dev_inst *sdi;
	struct sr_scpi_dev_inst *scpi;
	struct dev_context *devc;
	struct scpi_dmm_acq_info *info;
	gboolean sent_sample;
	size_t ch;
	struct sr_channel *channel;
	int ret;

	(void)fd;
	(void)revents;

	sdi = cb_data;
	if (!sdi)
		return TRUE;
	scpi = sdi->conn;
	devc = sdi->priv;
	if (!scpi || !devc)
		return TRUE;
	info = &devc->run_acq_info;

	sent_sample = FALSE;
	ret = SR_OK;
	for (ch = 0; ch < devc->num_channels; ch++) {
		/* Check the channel's enabled status. */
		channel = g_slist_nth_data(sdi->channels, ch);
		if (!channel->enabled)
			continue;

		/*
		 * Prepare an analog measurement value. Note that digits
		 * will get updated later.
		 */
		info->packet.type = SR_DF_ANALOG;
		info->packet.payload = &info->analog[ch];
		sr_analog_init(&info->analog[ch], &info->encoding[ch],
			&info->meaning[ch], &info->spec[ch], 0);

		/* Just check OPC before sending another request. */
		scpi_dmm_cmd_delay(sdi->conn);

		/*
		 * Have the model take and interpret a measurement. Lack
		 * of support is pointless, failed retrieval/conversion
		 * is considered fatal. The routine will fill in the
		 * 'analog' details, except for channel name and sample
		 * count (assume one value per channel).
		 *
		 * Note that non-zero non-negative return codes signal
		 * that the channel's data shell get skipped in this
		 * iteration over the channels. This copes with devices
		 * or modes where channels may provide data at different
		 * rates.
		 */
		if (!devc->model->get_measurement) {
			ret = SR_ERR_NA;
			break;
		}
		ret = devc->model->get_measurement(sdi, ch);
		if (ret > 0)
			continue;
		if (ret != SR_OK)
			break;

		/* Send the packet that was filled in by the model's routine. */
		info->analog[ch].num_samples = 1;
		info->analog[ch].meaning->channels = g_slist_append(NULL, channel);
		sr_session_send(sdi, &info->packet);
		g_slist_free(info->analog[ch].meaning->channels);
		sent_sample = TRUE;
	}
	if (sent_sample)
		sr_sw_limits_update_samples_read(&devc->limits, 1);
	if (ret != SR_OK) {
		/* Stop acquisition upon communication or data errors. */
		sr_dev_acquisition_stop(sdi);
		return TRUE;
	}
	if (sr_sw_limits_check(&devc->limits))
		sr_dev_acquisition_stop(sdi);

	return TRUE;
}
