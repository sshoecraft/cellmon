
/*
Copyright (c) 2021, Stephen P. Shoecraft
All rights reserved.

This source code is licensed under the BSD-style license found in the
LICENSE file in the root directory of this source tree.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <ctype.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <pwd.h>
#include <time.h>
#include "parson.h"
#include "mybmm.h"
//#include "jbd.h"
#include "mqtt.h"

/* Default refresh rate */
#define DEFAULT_INTERVAL 1

#ifndef TRUE
#define TRUE 1
#endif

int debug = 0;

void display(struct mybmm_config *conf);

int init_pack(mybmm_pack_t *pp, mybmm_config_t *c, char *type, char *transport, char *target, char *opts, mybmm_module_t *cp, mybmm_module_t *tp) {
	memset(pp,0,sizeof(*pp));
	strcpy(pp->type,type);
	if (transport) strcpy(pp->transport,transport);
	if (target) strcpy(pp->target,target);
	if (opts) strcpy(pp->opts,opts);
        pp->open = cp->open;
        pp->read = cp->read;
        pp->close = cp->close;
        pp->handle = cp->new(c,pp,tp);
	list_add(c->packs,pp,sizeof(*pp));
        return 0;
}

mybmm_pack_t *find_pack_by_uuid(mybmm_config_t *conf, char *uuid) {
	mybmm_pack_t *pp;

	dprintf(1,"uuid: %s\n", uuid);
	list_reset(conf->packs);
	while((pp = list_get_next(conf->packs)) != 0) {
		if (strcmp(pp->uuid,uuid) == 0) {
			dprintf(1,"found!\n");
			return pp;
		}
	}
	dprintf(1,"NOT found!\n");
	return 0;
}

mybmm_pack_t *find_pack_by_name(mybmm_config_t *conf, char *name) {
	mybmm_pack_t *pp;

	dprintf(1,"name: %s\n", name);
	list_reset(conf->packs);
	while((pp = list_get_next(conf->packs)) != 0) {
		if (strcmp(pp->name,name) == 0) {
			dprintf(1,"found!\n");
			return pp;
		}
	}
	dprintf(1,"NOT found!\n");
	return 0;
}

void get_temps(mybmm_pack_t *pp, JSON_Array *array) {
	int i,count;
	JSON_Value *value;
	float f;

	count = json_array_get_count(array);
	for(i=0; i < count; i++) {
		value = json_array_get_value(array, i);
		f = json_value_get_number(value);
		dprintf(1,"i: %d, f: %f\n", i, f);
#define FTEMP(v) ( ( ( (float)(v) * 9.0 ) / 5.0) + 32.0)
		pp->temps[i] = FTEMP(f);
	}
	pp->ntemps = count;
}

void get_cellvolts(mybmm_pack_t *pp, JSON_Array *array) {
	int i,count;
	JSON_Value *value;
	float f;

	count = json_array_get_count(array);
	for(i=0; i < count; i++) {
		value = json_array_get_value(array, i);
		f = json_value_get_number(value);
		dprintf(1,"i: %d, f: %f\n", i, f);
		pp->cellvolt[i] = f;
	}
	pp->cells = count;
}

int _namesort(list_item item1, list_item item2) {
	mybmm_pack_t *p1 = (mybmm_pack_t *)item1->item;
	mybmm_pack_t *p2 = (mybmm_pack_t *)item2->item;
	int val;

	dprintf(7,"p1: %s, p2: %s\n", p1->name, p2->name);
	val = strcmp(p1->name,p2->name);
	if (val < 0)
		val =  -1;
	else if (val > 0)
		val = 1;
	dprintf(7,"returning: %d\n", val);
	return val;

}

void dump_pack(mybmm_pack_t *pp) {
	register int i;

	dprintf(1,"%s: uuid: %s",pp->name,pp->uuid);
	dprintf(1,"%s: capacity: %3.3f\n",pp->name,pp->capacity);
	dprintf(1,"%s: voltage: %3.3f\n",pp->name,pp->voltage);
	dprintf(1,"%s: current: %3.3f\n",pp->name,pp->current);
	dprintf(1,"%s: status: %d\n",pp->name,pp->status);
	dprintf(1,"%s: ntemps: %d\n",pp->name,pp->ntemps);
	for(i=0; i < pp->ntemps; i++) dprintf(1,"%s: temps[%d]: %3.3f\n",pp->name,i,pp->temps[i]);
	dprintf(1,"%s: cells: %d\n",pp->name,pp->cells);
	for(i=0; i < pp->cells; i++) dprintf(1,"%s: cellvolt[%d]: %3.3f\n",pp->name,pp->cellvolt[i]);
}

void get_state(mybmm_pack_t *pp, char *str) {
	int i;
	char *p;

	dprintf(1,"str: %s\n", str);

	i=0;
	while(1) {
		p = strele(i++,",",str);
		if (!strlen(p)) break;
		dprintf(1,"p: %s\n", p);
		if (strcmp(p,"Charging") == 0)
			mybmm_set_state(pp,MYBMM_PACK_STATE_CHARGING);
		else if (strcmp(p,"Discharging") == 0)
			mybmm_set_state(pp,MYBMM_PACK_STATE_DISCHARGING);
		else if (strcmp(p,"Balancing") == 0)
			mybmm_set_state(pp,MYBMM_PACK_STATE_BALANCING);
	}
}

void get_balance(mybmm_pack_t *pp, char *str) {
	int i,len;
	unsigned long mask;

	dprintf(1,"str: %s\n", str);

	mask = 1;
	len = strlen(str);
	for(i=0; i < len; i++) {
		if (str[i] == '1') pp->balancebits |= mask;
		mask <<= 1;
	}
	dprintf(1,"%04x\n", pp->balancebits);
}

void add_or_update_pack(mybmm_config_t *conf, char *name, char *payload) {
	mybmm_pack_t pack,*pp;
	JSON_Object *object;
	JSON_Value *root,*value;
	int i,count,type,isnew;
	char *label, *valp;
	double num;

	dprintf(1,"conf: %p, name: %s, data: %s\n", conf, name, payload);

	dprintf(1,"parsing...\n");
	root = json_parse_string(payload);
	if (json_value_get_type(root) != JSONObject) {
		printf("error: not a valid json string\n");
		return;
	}
	object = json_value_get_object(root);
	count  = json_object_get_count(object);
	/* First, try to find the uuid */
	pp = 0;
	for (i = 0; i < count; i++) {
		label = (char *)json_object_get_name(object, i);
		if (!label) continue;
		if (strcmp(label,"uuid") == 0) {
			value = json_object_get_value(object, label);
			pp = find_pack_by_uuid(conf,(char *)json_value_get_string(value));
			break;
		}
	}
	if (!pp) pp = find_pack_by_name(conf,name);
	if (!pp) {
		memset(&pack,0,sizeof(pack));
		pp = &pack;
		strcpy(pack.name,name);
		isnew = 1;
	} else
		isnew = 0;
	for (i = 0; i < count; i++) {
		label = (char *)json_object_get_name(object, i);
		if (!label) {
			printf("error getting object name\n");
			return;
		}
		dprintf(3,"label: %s\n", label);
		value = json_object_get_value(object, label);
		type = json_value_get_type(value);
		switch(type) {
		case JSONString:
			valp = (char *)json_value_get_string(value);
			dprintf(1,"string: %s\n", valp);
			if (strcmp(label,"name") == 0)
				strcpy(pp->name,valp);
			else if (strcmp(label,"uuid") == 0)
				strcpy(pp->uuid,valp);
			else if (strcmp(label,"states") == 0)
				get_state(pp,valp);
			else if (strcmp(label,"balancebits") == 0)
				get_balance(pp,valp);
			break;
		case JSONNumber:
			num = json_value_get_number(value);
			dprintf(1,"num: %f\n", num);
			if (strcasecmp(label,"Voltage")==0)
				pp->voltage = num;
			else if (strcasecmp(label,"Current")==0)
				pp->current = num;
			else if (strcasecmp(label,"Capacity")==0)
				pp->capacity = num;
			else if (strcasecmp(label,"Status")==0)
				pp->status = num;
			break;
		case JSONArray:
			if (strcmp(label,"Temps")==0 || strcmp(label,"temps")==0)
				get_temps(pp,json_value_get_array(value));
			else if (strcmp(label,"cellvolt")==0 || strcmp(label,"Cells")==0)
				get_cellvolts(pp,json_value_get_array(value));
#if 0
			else if (strcmp(label,"cellres")==0)
				get_cellres(pp,json_value_get_array(value));
#endif
			else {
				printf("unknown array: %s\n",label);
			}
			break;
		default:
			printf("error: bad type in json file: %d\n", type);
			break;
		}
	}
	mybmm_set_state(pp,MYBMM_PACK_STATE_UPDATED);
	time(&pp->last_update);
//	dprintf(1,"isnew: %d\n", isnew);
	if (isnew) {
//		dump_pack(pp);
		list_add(conf->packs,pp,sizeof(*pp));
		list_sort(conf->packs,_namesort,0);
	}
//	dprintf(1,"returning...\n");
	conf->state = 1;
}

int mycb(void *ctx, char *topicName, int topicLen, MQTTClient_message *message) {
	int len;
	char topic[128],payload[1024],name[32];

	dprintf(1,"ctx: %p, topicName: %p, topicLen: %d, message: %p\n", ctx, topicName, topicLen, message);
	if (topicLen) {
		len = topicLen > sizeof(topic)-1 ? sizeof(topic)-1 : topicLen;
		memcpy(topic,topicName,len);
		topic[len] = 0;
	} else {
		topic[0] = 0;
		strncat(topic,topicName,sizeof(topic)-1);
	}
	dprintf(1,"topic: %s\n",topic);
	len = message->payloadlen > sizeof(payload)-1 ? sizeof(payload)-1 : message->payloadlen;
	memcpy(payload,message->payload,len);
        payload[len] = 0;
	dprintf(5,"payload: %s\n", payload);
	/* /SolarD/Battery/<name>/Data */
	name[0] = 0;
	strncat(name,strele(3,"/",topic),sizeof(name)-1);
	dprintf(1,"name: %s\n", name);
	add_or_update_pack(ctx,name,payload);
	MQTTClient_freeMessage(&message);
	MQTTClient_free(topic);
	dprintf(1,"returning...\n");
	return TRUE;
}

char *find_config_file(char *name) {
	static char temp[1024];
	long uid;
	struct passwd *pw;

	if (access(name,R_OK)==0) return name;
	uid = getuid();
	pw = getpwuid(uid);
	if (pw) {
		sprintf(temp,"%s/etc/%s",pw->pw_dir,name);
		dprintf(1,"temp: %s\n", temp);
		if (access(temp,R_OK)==0) return temp;
	}

	sprintf(temp,"/etc/%s",name);
	if (access(temp,R_OK)==0) return temp;
	sprintf(temp,"/usr/local/etc/%s",name);
	if (access(temp,R_OK)==0) return temp;
	sprintf(temp,"/opt/mybmm/etc/%s",name);
	if (access(temp,R_OK)==0) return temp;
	return 0;
}

void usage(char *name) {
	printf("usage: %s [-acjJrwlh] [-f filename] [-b <bluetooth mac addr | -i <ip addr>] [-o output file]\n",name);
	printf("arguments:\n");
#ifdef DEBUG
	printf("  -d <#>	debug output\n");
#endif
	printf("  -h		this output\n");
}

int main(int argc, char **argv) {
	int opt;
	char type[MYBMM_MODULE_NAME_LEN+1],transport[MYBMM_MODULE_NAME_LEN+1],target[MYBMM_TARGET_LEN+1],*opts;
	mybmm_config_t *conf;
	mqtt_session_t *m;

	opts = 0;
	type[0] = transport[0] = target[0] = 0;
	while ((opt=getopt(argc, argv, "d:i:h")) != -1) {
		switch (opt) {
		case 'd':
			debug=atoi(optarg);
			break;
		case 'h':
		default:
			usage(argv[0]);
			exit(0);
                }
	}

	/* Create the config */
	conf = calloc(sizeof(*conf),1);
	if (!conf) {
		perror("calloc conf");
		return 1;
	}
	conf->modules = list_create();
	conf->packs = list_create();

	dprintf(2,"type: %s\n", type);
	if (strlen(type)) {
		/* Single pack */
		mybmm_module_t *cp,*tp;
		mybmm_pack_t pack;

		tp = mybmm_load_module(conf,transport,MYBMM_MODTYPE_TRANSPORT);
		if (!tp) return 1;
		cp = mybmm_load_module(conf,type,MYBMM_MODTYPE_CELLMON);
		if (!cp) return 1;
		if (init_pack(&pack,conf,type,transport,target,opts,cp,tp)) return 1;
	} else {
		/* Read from config */
		conf->filename = find_config_file("cellmon.conf");
		if (!conf->filename) {
			printf("error: unable to find cellmon.conf");
			return 1;
		}
		conf->cfg = cfg_read(conf->filename);
		dprintf(3,"cfg: %p\n", conf->cfg);
		if (!conf->cfg) {
			printf("error: unable to read config file '%s': %s\n", conf->filename, strerror(errno));
			return 1;
		}
		mqtt_init(conf);
		pack_init(conf);
	}

	if (strlen(conf->mqtt_broker)) {
		char topic[192];

		sprintf(topic,"%s/#",conf->mqtt_topic);
		m = mqtt_new(conf->mqtt_broker,"Cellmon",topic);
		if (mqtt_setcb(m,conf,0,mycb,0)) return 1;
		if (mqtt_connect(m,20,conf->mqtt_username,conf->mqtt_password)) return 1;
		if (mqtt_sub(m,topic)) return 1;
		if (mqtt_sub(m,"/SolarD/Battery/+/Data")) return 1;
	}

	/* main loop */
	conf->cell_crit_high = 9.9;
	while(1) {
		display(conf);
		if (debug) {
			conf->state = 0;
			while(!conf->state) sleep(1);
		} else {
			sleep(1);
		}
	}

	return 0;
}
