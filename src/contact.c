/*
 *  Copyright (c) 2014 Christoph Mahrl
 *
 *  This file is part of DChat.
 *
 *  DChat is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  DChat is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with DChat.  If not, see <http://www.gnu.org/licenses/>.
 */


/** @file contact.c
 *  This file contains functions concerning dchat contacts for adding, deleting,
 *  searching, sending and receiving contacts.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dchat_h/contact.h"
#include "dchat_h/types.h"
#include "dchat_h/decoder.h"
#include "dchat_h/dchat.h"
#include "dchat_h/util.h"
#include "dchat_h/consoleui.h"


/**
 *  Sends local contactlist to a contact.
 *  Sends all known contacts stored in the contactlist within the global config
 *  in form of a "control/discover" PDU to the given contact.
 *  @param n   Index of contact to whom we send our contactlist (excluding him)
 *  @return amount of bytes that have been written as content, -1 on error
 */
int
send_contacts(int n)
{
    dchat_pdu_t pdu;    // pdu with contact information
    char* contact_str;  // pointer to a string representation of a contact
    int i;
    int ret;            // return value
    int pdu_len = 0;    // total content length of pdu-packet that will be sent
    contact_t* contact; // contact that will be converted to a string
    // initialize PDU
    init_dchat_pdu(&pdu, 1.0, CTT_ID_DSC, _cnf->me.onion_id, _cnf->me.lport,
                   _cnf->me.name);

    // iterate through our contactlist
    for (i = 0; i < _cnf->cl.cl_size; i++)
    {
        // except client n to whom we sent our contactlist
        if (n == i)
        {
            continue;
        }

        // temporarily point to a contact
        contact = &(_cnf->cl.contact[i]);

        // if its not an empty contact slot and contact is not temporary (has not sent "control/discover" yet)
        if (contact->lport != 0)
        {
            // convert contact to a string
            if ((contact_str = contact_to_string(contact)) == NULL)
            {
                ui_log(LOG_WARN, "Conversion of contact '%s' to string failed! - Skipped",
                       contact->name);
                continue;
            }

            // (re)allocate memory for pdu for contact string
            pdu.content = realloc(pdu.content, pdu_len + strlen(contact_str) + 1);
            pdu.content[pdu_len] = '\0'; // set the first byte to \0.. used for strcat

            // could not allocate memory for content
            if (pdu.content == NULL)
            {
                ui_fatal("Memory reallocation for contactlist failed!");
            }

            // add contact information to content
            strncat(pdu.content, contact_str, strlen(contact_str));
            // increase size of pdu content-length
            pdu_len += strlen(contact_str);
            free(contact_str);
        }
    }

    // set content-length of this pdu
    pdu.content_length = pdu_len;

    // write pdu inkluding all addresses of our contacts
    if ((ret = write_pdu(_cnf->cl.contact[n].fd, &pdu)) == -1)
    {
        ui_log(LOG_ERR, "Sending of contactlist failed!");
    }

    if (pdu.content_length != 0)
    {
        free(pdu.content);
    }

    return ret;
}


/**
 *  Contacts transferred via PDU will be added to the contactlist.
 *  Parses the contact information stored in the given PDU, connects to this remote client
 *  , if this client is unknown, the local contactlist within the global config will be sent
 *  to him. Finally the remote client is added as contact to the local contactlist.
 *  For every parsed contact information, this procedure is repeated.
 *  @param pdu PDU with the contact information in its content
 *  @return amount of new contacts added to the contactlist, -1 on error
 */
int
receive_contacts(dchat_pdu_t* pdu)
{
    contact_t contact;
    int ret = 0;            // return value
    int new_contacts = 0;   // stores how many new contacts have been received
    int known_contacts = 0; // stores how many known contacts have been received
    int line_begin = 0;     // offset of content of given pdu
    int line_end = 0;       // offset (end) of content of given pdu
    char* line;             // contact string

    // as long as the line_end index is lower than content-length
    while (line_end < pdu->content_length)
    {
        line_begin = line_end;

        // extract a line from the content of the pdu
        if ((line_end = get_content_part(pdu, line_begin, '\n', &line)) == -1)
        {
            ui_log(LOG_ERR, "Extraction of contact line from received PDU failed!");
            ret = -1;
            break;
        }

        // parse line ane make string to contact
        if (string_to_contact(&contact, line) == -1)
        {
            ui_log(LOG_WARN, "Conversion of string to contact failed! - Skipped");
            ret = -1;
            continue;
        }

        // if parsed contact is unknown
        if (find_contact(&contact, 0) == -2)
        {
            // increment new contacts counter
            new_contacts++;

            // connect to new contact, add him as contact, and send contactlist to him
            if (handle_local_conn_request(contact.onion_id, contact.lport) == -1)
            {
                ui_log(LOG_WARN, "Connection to new contact failed!");
                ret = -1;
            }
        }
        else
        {
            // we found parsed contact in contactlist -> increment known contacts counter
            known_contacts++;
        }

        // increment end of line index, to point to the beginning of the next line
        line_end++;
    }

    return ret != -1 ? new_contacts : -1;
}


/**
 *  Checks the local contactlist for duplicates.
 *  Checks if there are duplicate contacts in the contactlist. Contacts
 *  with the same listening port and ip address are considered as duplicate.
 *  If there are duplicates, the index of the contact which should be deleted
 *  will be returned. This function implements the duplicate detection
 *  mechanismn of the DChat protocol. Therefore for further information read
 *  the DChat protocol specification for detecting and removing duplicates.
 *  @see find_contact()
 *  @param n   Index of contact to check for duplicates
 *  @return index of duplicate, -1 if there are no duplicates or if contact
 *  is not in the contactlist
 */
int
check_duplicates(int n)
{
    int fst_oc;              // index of first occurance of contact
    int sec_oc;              // index of duplicate
    contact_t* temp;         // temporay contact variable
    int connect_contact = 0; // index of contact to whom we connected to
    int accept_contact = 0;  // index of contact from whom we accepted a connection
    int ret;
    // check if given contact is in the contactlist
    fst_oc = find_contact(&_cnf->cl.contact[n], 0);

    // contact is this client
    if (fst_oc == -1)
    {
        return n;
    }

    if (fst_oc == -2)
    {
        return -1; // contact not in list
    }

    // check if given contact is in the contactlist a second time
    sec_oc = find_contact(&_cnf->cl.contact[n], fst_oc + 1);

    if (sec_oc == -2)
    {
        return -1; // no duplicate contact
    }

    // extract port of sockaddr_storage structure
    temp = &_cnf->cl.contact[fst_oc];

    // which kind of contact has to be deleted?
    if (temp->accepted)
    {
        accept_contact = fst_oc;
        connect_contact = sec_oc;
    }
    else
    {
        connect_contact = fst_oc;
        accept_contact = sec_oc;
    }

    // if local onion address is greater than the remote one
    // than the index of the  contact, who got added because of a "connect",
    // will be returned
    ret = strcmp(_cnf->me.onion_id, _cnf->cl.contact[n].onion_id);

    if (ret > 0)
    {
        return connect_contact;
    }
    // otherwise it is the other way round
    else if (ret < 0)
    {
        return accept_contact;
    }
    // if ip addresses are equal, do the same for the listening port
    else if (_cnf->me.lport > _cnf->cl.contact[n].lport)
    {
        return connect_contact;
    }
    else if (_cnf->me.lport < _cnf->cl.contact[n].lport)
    {
        return accept_contact;
    }
    else
    {
        return accept_contact;
    }
}


/**
 *  Converts a contact into a string.
 *  This function converts a contact into a string representation. The string
 *  of the contact will then be returned and should be freed.
 *  @param contact Pointer to contact that should be converted to a string
 *  @return String representation of contact, NULL on error
 */
char*
contact_to_string(contact_t* contact)
{
    char* contact_str; // pointer to string repr. of contact
    char port_str[MAX_INT_STR + 1]; // max. characters of an int
    int contact_len; // length of contact structure

    if (!is_valid_onion(contact->onion_id))
    {
        return NULL;
    }

    if (!is_valid_port(contact->lport))
    {
        return NULL;
    }

    // convert port to a string
    snprintf(port_str, MAX_INT_STR, "%u", contact->lport);
    port_str[5] = '\0';
    // +4 for two " ", \n and \0
    contact_len = strlen(contact->onion_id) + strlen(port_str) + 4;

    // allocate memory for the contact string repr.
    if ((contact_str = malloc(contact_len)) == NULL)
    {
        ui_fatal("Memory allocaction for contact string failed!");
    }

    // first byte has to be '\0', otherwise strcat is undefined
    contact_str[0] = '\0';
    // create contact string like this:
    // <ip address> <port>\n
    // (see: DChat Protocol - Contact Exchange
    strncat(contact_str, contact->onion_id, strlen(contact->onion_id));
    strncat(contact_str, " ", 1);
    strncat(contact_str, port_str, strlen(port_str));
    strncat(contact_str, "\n", 1);
    return contact_str;
}


/**
 *  Converts a string into a contact.
 *  Converts a string containing contact information into a contact structure.
 *  The string has to be in the form of: <onion-id> <port>\n
 *  Further details can be found in the DChat protocol specification
 *  @see contact_to_string()
 *  @param contact: Pointer to contact that should be converted to a string
 *  @return 0 if conversion was successful, -1 on error
 */
int
string_to_contact(contact_t* contact, char* string)
{
    char* onion_id; // onion address string (splitted from line)
    char* port;     // port string (splitted from line)
    int lport;      // converted listening port
    char* ptr;      // used for strtol
    char* save_ptr; // pointer for strtok_r

    // split onion-id from string
    if ((onion_id = strtok_r(string, " ", &save_ptr)) == NULL)
    {
        ui_log(LOG_ERR, "Missing onion-id in contact string!");
        return -1;
    }

    // split port from string
    if ((port = strtok_r(NULL, "\n", &save_ptr)) == NULL)
    {
        ui_log(LOG_ERR, "Missing listening port in contact string!");
        return -1;
    }

    if (!is_valid_onion(onion_id))
    {
        ui_log(LOG_ERR, "Invalid onion-id of contact string!");
        return -1;
    }

    // could line be splittet into ip and port?
    // convert port string to integer
    lport = (int) strtol(port, &ptr, 10);

    if (ptr[0] != '\0' || !is_valid_port(lport))
    {
        ui_log(LOG_ERR, "Invalid port of contact string!");
        return -1;
    }

    contact->lport = lport;
    contact->onion_id[0] = '\0';
    strncat(contact->onion_id, onion_id, ONION_ADDRLEN);
    return 0;
}


/**
 *  Resizes the contactlist.
 *  Function to resize the contactlist to a given size. Old contacts are copied to the new
 *  resized contact list if they fit in it
 *  @param newsize New size of the contactlist
 *  @return 0 on success, -1 on error
 */
int
realloc_contactlist(int newsize)
{
    int i, j = 0;
    contact_t* new_contact_list;
    contact_t* old_contact_list;

    // size may not be lower than 1 and not be lower than the amount of contacts actually used
    if (newsize < 1 || newsize < _cnf->cl.used_contacts)
    {
        ui_log(LOG_ERR,
               "New size of contactlist must not be lower than 1 or the amount of contacts actually stored in the contactlist!");
        return -1;
    }

    // pointer to beginning of old contactlist
    old_contact_list = &(_cnf->cl.contact[0]);

    // reserve memory for new contactlist
    if ((new_contact_list = malloc(newsize * sizeof(contact_t))) == NULL)
    {
        ui_fatal("Reallocation of contactlist failed!");
    }

    // zero out new contactlist
    memset(new_contact_list, 0, newsize * sizeof(contact_t));

    // copy contacts to new contactlist
    for (i = 0; i < _cnf->cl.cl_size; i++)
    {
        if (old_contact_list[i].fd)
        {
            memcpy(new_contact_list + j, old_contact_list + i, sizeof(contact_t));
            j++;
        }
    }

    // set new size and point to new contactlist in the global config
    _cnf->cl.cl_size = newsize;
    _cnf->cl.contact = new_contact_list;
    // free old contactlist
    free(old_contact_list);
    return 0;
}


/**
 *  Adds a new contact to the local contactlist.
 *  The given socket descriptor of the remote client will be used to add a new contact
 *  to the contactlist holded by the global config.
 *  @param fd  Socket file descriptor of the new contact
 *  @return index of contact list, where new contact has been added or -1 in case
 *          of error
 */
int
add_contact(int fd)
{
    int i;

    // if contactlist is full - resize it so that we can store more contacts in it
    if (_cnf->cl.used_contacts == _cnf->cl.cl_size)
    {
        if ((realloc_contactlist(_cnf->cl.cl_size + INIT_CONTACTS)) < 0)
        {
            return -1;
        }
    }

    // search for an empty place to store the new contact
    for (i = 0; i < _cnf->cl.cl_size; i++)
    {
        // if fd is 0 -> this place can be used to store a new contact
        if (!_cnf->cl.contact[i].fd)
        {
            _cnf->cl.contact[i].fd = fd;
            _cnf->cl.used_contacts++; // increase contact counter
            break;
        }
    }

    // return index where contact has been stored
    return i;
}


/**
 *  Deletes a contact from the local contactlist.
 *  Deletes a contact from the contact list holded by the global config.
 *  @param n   Index of customer in the customer list
 *  @return 0 on success, -1 if index is out of bounds
 */
int
del_contact(int n)
{
    // is index 'n' a valid index?
    if ((n < 0) || (n >= _cnf->cl.cl_size))
    {
        ui_log(LOG_ERR, "del_contact() - Index out of bounds '%d'", n);
        return -1;
    }

    if (_cnf->cl.contact[n].fd == 0)
    {
        return 0;
    }

    close(_cnf->cl.contact[n].fd);
    // zero out the contact on index 'n'
    memset(&_cnf->cl.contact[n], 0, sizeof(contact_t));
    // decrease contacts counter variable
    _cnf->cl.used_contacts--;

    // if contacts counter has been decreased INIT_CONTACTS time, resize the contactlist to free
    // unused memory
    if ((_cnf->cl.used_contacts == (_cnf->cl.cl_size - INIT_CONTACTS)) &&
        _cnf->cl.used_contacts != 0)
    {
        if ((realloc_contactlist(_cnf->cl.cl_size - INIT_CONTACTS)) < 0)
        {
            return -1;
        }
    }

    return 0;
}

/**
 *  Searches a contact in the local contactlist.
 *  Searches for a contact in the contactlist holded within the global config
 *  and returns its index in the contactlist. To find a contact, its onion address
 *  and listening port will be used and compared with each other.
 *  @param contact Pointer to contact to search for
 *  @param begin   Index where the search will begin in the contactlist
 *  @return index of contact, -1 if the contact represents ourself, -2 if not found
 */
int
find_contact(contact_t* contact, int begin)
{
    int i;
    contact_t* c;

    // is begin a valid index?
    if (begin < 0 || begin >= _cnf->cl.cl_size)
    {
        return -2;
    }

    // iterate through contactlist, but first check
    // if the contact represents ourself
    for (i = -1; i < _cnf->cl.cl_size; i++)
    {
        // first check if the given contact matches ourself
        if (i == -1)
        {
            c = &_cnf->me;
            i = begin - 1;
        }
        else
        {
            c = &_cnf->cl.contact[i];
        }

        // dont check empty contacts or temporary contacts
        if (c->lport)
        {
            if (strcmp(contact_to_string(contact), contact_to_string(c)) == 0)
            {
                return i;
            }
        }
    }

    return -2; // not found
}
