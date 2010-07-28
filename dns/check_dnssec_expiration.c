/***
 * Monitoring Plugin - check_dnssec_expiration
 **
 *
 * check_dnssec_expiration - Check if the zone signature expires.
 * Copyright (C) 2010 Marius Rieder <marius.rieder@durchmesser.ch>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * $Id$
 */

const char *progname  = "check_dnssec_expiration";
const char *progvers  = "0.1";
const char *progcopy  = "2009 - 2010";
const char *progauth = "Marius Rieder <marius.rieder@durchmesser.ch>";
const char *progusage = "[-H host] -D domain [-k file] [-t timeout] [-w warn] [-c crit]";

#include "mp_common.h"
#include "dns_utils.h"

#include <getopt.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <ldns/ldns.h>

/* Global vars */
char *hostname = NULL;
char *domainname = NULL;
ldns_rr_list *trusted_keys = NULL;
thresholds *exp_thresholds = NULL;

int main (int argc, char **argv) {
    
    /* C vars */
    int         i;
    int         longest = 0;
    uint32_t    exp, ttl, now;
    
    /* LDNS vars */
    ldns_rdf        *rd_domain;
    ldns_pkt        *pkt;
    ldns_resolver   *res;
    ldns_rdf        *rd;
    ldns_rr         *rr;
    ldns_rr         *rr_newest = NULL;
    ldns_rr_list    *rrl_keys;
    ldns_rr_list    *rrl_soa;
    ldns_rr_list    *rrl_soa_rrsig;
    ldns_status	    status;
    
    /* Set signal handling and alarm */
    if (signal(SIGALRM, timeout_alarm_handler) == SIG_ERR)
        unknown("Cannot catch SIGALRM");

    /* Set Default range */
    setWarnTime(&exp_thresholds, "2d:");
    setCritTime(&exp_thresholds, "1d:");

    /* Parse argumens */
    if (process_arguments (argc, argv) == ERROR)
        unknown("Could not parse arguments");
    
    /* Start plugin timeout */
    alarm(mp_timeout);
    
    rd_domain = ldns_dname_new_frm_str(domainname);
    if (!rd_domain)
        unknown("Illegal domain name");
    
    /* Create a new resolver with hostname or server from /etc/resolv.conf */
    res = createResolver(hostname);
    if (!res)
        unknown("Creating resolver faild.");
    resolverEnableDnssec(res);
        
    /* Query for soa */
    pkt = ldns_resolver_query(res, rd_domain, LDNS_RR_TYPE_SOA,
                              LDNS_RR_CLASS_IN, LDNS_RD);
    if (!pkt) {
        ldns_rdf_deep_free(rd_domain);
        ldns_resolver_deep_free(res);
        unknown("error pkt sending");
    }
    
    rrl_soa = ldns_pkt_rr_list_by_name_and_type(pkt, rd_domain,
                LDNS_RR_TYPE_SOA, LDNS_SECTION_ANSWER);

    if (!rrl_soa) {
        ldns_rdf_deep_free(rd_domain);
        ldns_resolver_deep_free(res); 
        ldns_pkt_free(pkt);
        unknown("Invalid answer after SOA query.");
    }

    rrl_soa_rrsig = ldns_dnssec_pkt_get_rrsigs_for_name_and_type(pkt,
                        rd_domain, LDNS_RR_TYPE_SOA);
    
    if (!rrl_soa_rrsig) {
        ldns_rdf_deep_free(rd_domain);
        ldns_resolver_deep_free(res); 
        ldns_pkt_free(pkt);
        ldns_rr_list_deep_free(rrl_soa);
        critical("SOA is not signed.");
    }
    
    if (mp_verbose >= 2) {
        printf("--[ Checked Domain ]----------------------------------------\n");
        ldns_rr_list_print(stdout, rrl_soa);
        ldns_rr_list_print(stdout, rrl_soa_rrsig);
        printf("------------------------------------------------------------\n");
    }
    
    ldns_pkt_free(pkt);
    
    /* Query for keys */
    if (ldns_rr_list_rr_count(trusted_keys) > 0) {
        rrl_keys = ldns_validate_domain_dnskey(res, rd_domain, trusted_keys);
        if (ldns_rr_list_rr_count(rrl_keys) == 0)
            rrl_keys = NULL;
    } else {
        pkt = ldns_resolver_query(res, rd_domain, LDNS_RR_TYPE_DNSKEY,
                                  LDNS_RR_CLASS_IN, LDNS_RD);
        if (!pkt) {
            ldns_rdf_deep_free(rd_domain);
            ldns_resolver_deep_free(res);
            ldns_rr_list_deep_free(rrl_soa);
            ldns_rr_list_deep_free(rrl_soa_rrsig);
            unknown("error pkt sending");
        }
        
        rrl_keys = ldns_pkt_rr_list_by_type(pkt, LDNS_RR_TYPE_DNSKEY,
                                            LDNS_SECTION_ANSWER);
        ldns_pkt_free(pkt);
    }
    
    if (!rrl_keys) {
        ldns_rdf_deep_free(rd_domain);
        ldns_resolver_deep_free(res);
        ldns_rr_list_deep_free(rrl_soa);
        ldns_rr_list_deep_free(rrl_soa_rrsig);
        critical("'%s' has no valid DNSKEYs.", domainname);
    }
    
    if (mp_verbose >= 2) {
        printf("--[ rrl_keys ]----------------------------------------\n");
        ldns_rr_list_print(stdout, rrl_keys);
        printf("------------------------------------------------------------\n");
    }
    
    /* Free rd_domain and resolver */
    ldns_rdf_deep_free(rd_domain);
    ldns_resolver_deep_free(res);
    
    /* Search longest valid rrsig */
    for(i = 0; i < ldns_rr_list_rr_count(rrl_soa_rrsig); i++) {
        rr = ldns_rr_list_rr(rrl_soa_rrsig, i);

        /* Check rrsigs */
        status = ldns_verify_rrsig_keylist(rrl_soa, rr, rrl_keys, NULL);
        if (status != LDNS_STATUS_OK)
            continue;

        ttl = ldns_rr_ttl(rr);
    
        rd = ldns_rr_rrsig_expiration(rr);
        exp = ldns_rdf2native_int32(rd);
    
        if ((exp-ttl) > longest) {
            if (rr_newest)
                ldns_rr_free(rr_newest);
            rr_newest = ldns_rr_clone(rr);
            longest = exp-ttl;
        }
    }

    /* Free lists */
    ldns_rr_list_deep_free(rrl_soa);
    ldns_rr_list_deep_free(rrl_soa_rrsig);
    ldns_rr_list_deep_free(rrl_keys);
    
    if (!rr_newest) {
        critical("SOA has no valid RRSIG.");
    }
    
    now = time(0);
    ttl = ldns_rr_ttl(rr_newest);
    rd = ldns_rr_rrsig_expiration(rr_newest);
    exp = ldns_rdf2native_int32(rd);
    char *exp_str = ldns_rdf2str(rd);
    ldns_rr_free(rr_newest);
        
    switch ( get_status((exp-ttl-now),exp_thresholds) ) {
        case STATE_OK:
            ok("RRSIG for %s valid till %s.", domainname, exp_str);
        case STATE_WARNING:
            warning("RRSIG for %s expires soon (%s).", domainname, exp_str);
        case STATE_CRITICAL:
            critical("RRSIG for %s expires to soon (%s).", domainname, exp_str);
            
    }
    
    critical("You should never reach this point.");
}

int process_arguments (int argc, char **argv) {
    int c;
    int option = 0;

    static struct option longopts[] = {
        MP_LONGOPTS_DEFAULT,
        MP_LONGOPTS_HOST,
        {"domain", required_argument, 0, 'D'},
        {"trusted-keys", required_argument, 0, 'k'},
        MP_LONGOPTS_WC,
        MP_LONGOPTS_TIMEOUT,
        MP_LONGOPTS_END
    };
   
    if (argc < 2) {
        print_help();
        exit(STATE_OK);
    }
    
    while (1) {
        c = getopt_long (argc, argv, MP_OPTSTR_DEFAULT"H:D:k:w:c:t:", longopts, &option);

        if (c == -1 || c == EOF)
            break;

        getopt_default(c);
        getopt_host_ip(c, optarg, &hostname);
        getopt_wc_time(c, optarg, &exp_thresholds);
        getopt_timeout(c, optarg);

        switch (c) {
            case 'D':
                if (!is_hostname(optarg))
                    usage("Illegal domain name.");
                domainname = optarg;
                break;
            case 'k':
                trusted_keys = loadKeyfile(optarg);
                if (trusted_keys == NULL)
                    usage("Parsing keyfiel faild.");
                break;
        }
    }
    
    if (!domainname)
        usage("A domainname is mandatory.");

    return(OK);
}

void print_help (void) {
    print_revision();
    print_copyright();

    printf("\n");
  
    printf ("This plugin check the RRSIG expiration date.\n\n");

    printf("\n\n");

    print_usage();

    print_help_default();
    print_help_host();
    printf(" -D, --domain=DOMAIN\n");
    printf("      The name of the domain to check.\n");
    printf(" -k, --trusted-keys=FILE\n");
    printf("      File to read trust-anchors from.\n");
    print_help_warn_time("2 days");
    print_help_crit_time("1 day");
    print_help_timeout();
}

/* vim: set ts=4 sw=4 et syn=c.libdns : */