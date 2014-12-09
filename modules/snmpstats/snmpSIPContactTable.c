/*
 * $Id$
 *
 * SNMPStats Module 
 * Copyright (C) 2006 SOMA Networks, INC.
 * Written by: Jeffrey Magder (jmagder@somanetworks.com)
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * Kamailio is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * Kamailio is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301
 * USA
 *
 * History:
 * --------
 * 2006-11-23 initial version (jmagder)
 * 
 * Note: this file originally auto-generated by mib2c using
 *        mib2c.array-user.conf 
 *
 * This file contains the implementation of the kamailioSIPContact Table.  For a
 * full description of this structure, please see the KAMAILIO-SIP-SERVER-MIB.
 *
 * Some important notes on implementation follow:
 *
 * We require Kamailios usrloc module to inform us when a contact is
 * added/removed.  The general callback process works as follows:
 *
 * 1) On startup, we register handleContactCallbacks() for USRLOC callbacks, so 
 *    we can be informed whenever a contact is added/removed from the system.  
 *    This registration happens with a call to registerForUSRLOCCallbacks().  
 *    (This is actually called when the SNMPStats module is initialized)
 *
 * 2) Whenever we receive a contact callback, handleContactCallbacks() will 
 *    quickly add the contact information and operation type to the
 *    interprocess buffer.  
 *
 * 3) When we receive an SNMP request for user/contact information, we consume
 *    the interprocess buffer with consumeInterprocessBuffer().  The function
 *    will add/delete rows to the tables, and then service the SNMP request.
 *
 * Notes: 
 *
 * - The interprocess buffer was necessary, because NetSNMP's containers can be
 *   very inefficient at adding large amounts of data at a time, such as when
 *   Kamailio first starts up.  It was decided its better to make an SNMP manager
 *   wait for data, instead of slowing down the rest of Kamailio while the
 *   sub-agent processes the data. 
 *
 * - It is important to send periodic SNMP requests to this table (or the user
 *   table), to make sure the process buffer doesn't get too large.  
 *
 */

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/agent/net-snmp-agent-includes.h>

#include <net-snmp/library/snmp_assert.h>

#include "hashTable.h"
#include "interprocess_buffer.h"
#include "utilities.h"
#include "snmpSIPContactTable.h"
#include "snmpstats_globals.h"

#include "../../mem/mem.h"
#include "../../str.h"
#include "../../sr_module.h"
#include "../../locking.h"
#include "../usrloc/usrloc.h"
#include "../usrloc/ucontact.h"


static netsnmp_handler_registration *my_handler = NULL;
static netsnmp_table_array_callbacks cb;

oid kamailioSIPContactTable_oid[]      = { kamailioSIPContactTable_TABLE_OID };
size_t kamailioSIPContactTable_oid_len = OID_LENGTH(kamailioSIPContactTable_oid);

/* 
 * This function adds a new contactToIndexStruct_t record to the front of
 * 'contactRecord'.  
 *
 * The structure is used to map a contact name to the SNMPStats modules integer
 * indexing scheme.  It will be used later when a delete command comes in, and
 * we need to find out which SNMP row the information is stored under.
 */
int insertContactRecord(
		contactToIndexStruct_t **contactRecord, int index, char *name) 
{
    int nameLength =strlen(name);
	
    contactToIndexStruct_t *newContactRecord = (contactToIndexStruct_t *)
        pkg_malloc(sizeof(contactToIndexStruct_t) +(nameLength+1)* sizeof(char));

	if (newContactRecord == NULL)
	{
        LM_ERR("no more pkg memory\n");
		return 0;
	}

	newContactRecord->next         = *contactRecord;
	newContactRecord->contactName  = (char*)newContactRecord + sizeof(contactToIndexStruct_t);
    memcpy(newContactRecord->contactName, name, nameLength);
    newContactRecord->contactName[nameLength]= '\0';
	newContactRecord->contactIndex = index;

	*contactRecord = newContactRecord;

	return 1;
}


/*
 * This function will remove the contactToIndexStruct_T record matching
 * 'contactName' from the users contactToIndexStruct_t linked-list, and return
 * the records index.  In the event that the record could not be found, 0 will
 * be returned. 
 */
int deleteContactRecord(contactToIndexStruct_t **contactRecord,char *contactName)
{
	int contactIndexToReturn;
	contactToIndexStruct_t *currentContact  = *contactRecord;
	contactToIndexStruct_t *previousContact = *contactRecord;

	while (currentContact != NULL) {

		if (strcmp(currentContact->contactName, contactName) == 0) 
		{
			/* This means that this is the first element.  Link up
			 * the pointer to the next element */
			if (currentContact == previousContact) {
				*contactRecord = currentContact->next;
			} else {
				previousContact->next = currentContact->next;
			}

			contactIndexToReturn = currentContact->contactIndex;
            pkg_free(currentContact);
			return contactIndexToReturn;
		}

		previousContact = currentContact;
		currentContact  = currentContact->next;

	}

	return 0;
}


/* 
 * Creates an SNMP row and inserts it into the contact table. This function
 * should only be called when the interprocess buffer is being consumed.
 *
 * Returns: 1 on success, and 0 otherwise. 
 */
int createContactRow(int userIndex, int contactIndex, char *contactName, 
		ucontact_t *contactInfo)  
{
	kamailioSIPContactTable_context *theRow;

	oid  *OIDIndex;
	int  stringLength;

	theRow = SNMP_MALLOC_TYPEDEF(kamailioSIPContactTable_context);

	if (theRow == NULL) {
		LM_ERR("failed to create a row for kamailioSIPContactTable\n");
		return 0;
	}

	/* We need enough memory for both the user index and contact index. */
	OIDIndex = pkg_malloc(sizeof(oid)*2);

	if (OIDIndex == NULL) {
		free(theRow);
		LM_ERR("failed to create a row for kamailioSIPContactTable\n");
		return 0;
	}

	stringLength = strlen(contactName);

	/* Generate the Rows Index */
	OIDIndex[0] = userIndex;
	OIDIndex[1] = contactIndex;

	theRow->index.len  = 2;
	theRow->index.oids = OIDIndex;
	theRow->kamailioSIPContactIndex = contactIndex;

	/* Fill in the rest of the rows columns */
	theRow->kamailioSIPContactURI = (unsigned char*)
        pkg_malloc((stringLength+ 1)* sizeof(char));
    if(theRow->kamailioSIPContactURI == NULL)
    {
        pkg_free(OIDIndex);
		free(theRow);
		LM_ERR("failed to allocate memory for contact name\n");
		return 0;
    }
    memcpy(theRow->kamailioSIPContactURI, contactName, stringLength);
    theRow->kamailioSIPContactURI[stringLength] = '\0';

    theRow->kamailioSIPContactURI_len = stringLength;
	theRow->contactInfo = contactInfo;

	CONTAINER_INSERT(cb.container, theRow);

	return 1;
}


/* 
 * Removes the row indexed by userIndex and contactIndex, and free's up the
 * memory allocated to it.  If the row could not be found, then nothing is done.
 */
void deleteContactRow(int userIndex, int contactIndex) 
{
	kamailioSIPContactTable_context *theRow;

	netsnmp_index indexToRemove;
	oid indexToRemoveOID[2];

	/* Form the OID Index of the row so we can search for it */
	indexToRemoveOID[0] = userIndex;
	indexToRemoveOID[1] = contactIndex;
	indexToRemove.oids  = indexToRemoveOID;
	indexToRemove.len   = 2;

	theRow = CONTAINER_FIND(cb.container, &indexToRemove);

	/* The ContactURI is shared memory, the index.oids was allocated from
	 * pkg_malloc(), and theRow was made with the NetSNMP API which uses
	 * malloc() */
	if (theRow != NULL) {
		CONTAINER_REMOVE(cb.container, &indexToRemove);
		pkg_free(theRow->kamailioSIPContactURI);
		pkg_free(theRow->index.oids);
		free(theRow);
	} 
}

/*
 * Initializes the kamailioSIPContactTable module.  This involves:
 *
 *  1) Registering the tables OID with the master agent
 *
 *  2) Creating a default row, so that there is a row to query to trigger the
 *     consumption of the interprocess buffer.
 */
void init_kamailioSIPContactTable(void)
{
	initialize_table_kamailioSIPContactTable();

	static char *defaultUser = "DefaultUser";

	createContactRow(1, 1, defaultUser, NULL);
}



/*
 * Initialize the kamailioSIPContactTable table by defining its contents and how
 * it's structured.
 *
 * This function is mostly auto-generated.
 */
void initialize_table_kamailioSIPContactTable(void)
{
	netsnmp_table_registration_info *table_info;

	if(my_handler) {
		snmp_log(LOG_ERR, "initialize_table_kamailioSIPContactTable_"
				"handler called again\n");
		return;
	}

	memset(&cb, 0x00, sizeof(cb));

	/** create the table structure itself */
	table_info = SNMP_MALLOC_TYPEDEF(netsnmp_table_registration_info);

	my_handler = netsnmp_create_handler_registration(
			"kamailioSIPContactTable",
			netsnmp_table_array_helper_handler,
			kamailioSIPContactTable_oid,
			kamailioSIPContactTable_oid_len,
			HANDLER_CAN_RONLY);
			
	if (!my_handler || !table_info) {
		snmp_log(LOG_ERR, "malloc failed in initialize_table_kamailio"
				"SIPContactTable_handler\n");
		return; /** mallocs failed */
	}

	/** index: kamailioSIPUserIndex */
	netsnmp_table_helper_add_index(table_info, ASN_UNSIGNED);
	/** index: kamailioSIPContactIndex */
	netsnmp_table_helper_add_index(table_info, ASN_UNSIGNED);

	table_info->min_column = kamailioSIPContactTable_COL_MIN;
	table_info->max_column = kamailioSIPContactTable_COL_MAX;

	/***************************************************
	 * registering the table with the master agent
	 */
	cb.get_value = kamailioSIPContactTable_get_value;
	cb.container = netsnmp_container_find("kamailioSIPContactTable_primary:"
			"kamailioSIPContactTable:" "table_container");
	
	DEBUGMSGTL(("initialize_table_kamailioSIPContactTable",
				"Registering table kamailioSIPContactTable "
				"as a table array\n"));
	
	netsnmp_table_container_register(my_handler, table_info, &cb, 
			cb.container, 1);
}


/*
 * This routine is called to process get requests for elements of the table.
 *
 * The function differs from its original auto-generated form in that the row
 * itself doesn't store all the data that is needed.  Some of the values
 * (kamailioSIPContactURI, kamailioSIPContactExpiry, kamailioSIPContactPreference)
 * may have changed since the row was first created.  Therefore, this data is
 * retrieved when it is requested for.
 */
int kamailioSIPContactTable_get_value(
			netsnmp_request_info *request,
			netsnmp_index *item,
			netsnmp_table_request_info *table_info )
{
	static char defaultExpiry[8]      = {0, 0, 0, 0, 0, 0, 0, 0};

	/* Needs to be large enough to hold the null terminated string "-0.01".
	 */
	char  contactPreference[6];	
	float preferenceAsFloat     = -1;

	char       *retrievedExpiry;
	struct tm  *timeValue;

	/* First things first, we need to consume the interprocess buffer, in
	 * case something has changed. We want to return the freshest data. */
	consumeInterprocessBuffer();

	netsnmp_variable_list *var = request->requestvb;

	kamailioSIPContactTable_context *context = 
		(kamailioSIPContactTable_context *)item;

	switch(table_info->colnum) 
	{

		case COLUMN_KAMAILIOSIPCONTACTDISPLAYNAME:

			/* FIXME: WHERE DO WE FIND THIS??  Setting to the same
			 * thing as contact uri for now. */
			snmp_set_var_typed_value(var, ASN_OCTET_STR, 
					(unsigned char*)
					context->kamailioSIPContactURI,
					context->kamailioSIPContactURI_len);
			break;

		case COLUMN_KAMAILIOSIPCONTACTURI:

			snmp_set_var_typed_value(var, ASN_OCTET_STR,
					 (unsigned char*)
					 context->kamailioSIPContactURI,
					 context->kamailioSIPContactURI_len);
			break;
	
		case COLUMN_KAMAILIOSIPCONTACTLASTUPDATED:
		
			if (context->contactInfo != NULL) 
			{
				timeValue = 
					localtime(&(context->contactInfo->last_modified));
				retrievedExpiry  = 
					convertTMToSNMPDateAndTime(timeValue);
			} else {
				retrievedExpiry = defaultExpiry;
			}

			snmp_set_var_typed_value(var, ASN_OCTET_STR,
					 (unsigned char*)
					 retrievedExpiry,
					 8);
			break;
	
		case COLUMN_KAMAILIOSIPCONTACTEXPIRY:

			if (context->contactInfo != NULL) 
			{
				timeValue = 
					localtime(&(context->contactInfo->expires));

				retrievedExpiry  = 
					convertTMToSNMPDateAndTime(timeValue);
			} else {
				retrievedExpiry = defaultExpiry;
			}
			snmp_set_var_typed_value(var, ASN_OCTET_STR,
					 (unsigned char*)
					 retrievedExpiry,
					 8);
			break;
	
		case COLUMN_KAMAILIOSIPCONTACTPREFERENCE:

			if (context->contactInfo != NULL) {
				preferenceAsFloat = context->contactInfo->q;
			}

			/* Kamailio stores the q-value as an integer for speed
			 * purposes.  We need to convert this value to a float
			 * to conform to the MIB and RFC specifications of the q
			 * value. */
			preferenceAsFloat /= 100.0;

			/* Convert the float into a string, as specified by the
			 * MIB. */
			sprintf(contactPreference, "%5.2f", preferenceAsFloat);

			snmp_set_var_typed_value(var, ASN_OCTET_STR,
					 (unsigned char*)
					 contactPreference,
					 5);
			break;
	
		default: /** We shouldn't get here */
			snmp_log(LOG_ERR, "unknown column in "
					 "kamailioSIPContactTable_get_value\n");
			return SNMP_ERR_GENERR;
	}

	return SNMP_ERR_NOERROR;
}

/* 
 * kamailioSIPContactTable_get_by_idx is an auto-generated function. 
 */
const kamailioSIPContactTable_context *
kamailioSIPContactTable_get_by_idx(netsnmp_index * hdr)
{
	return (const kamailioSIPContactTable_context *)
		CONTAINER_FIND(cb.container, hdr );
}


