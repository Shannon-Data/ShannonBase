/* Copyright (c) 2000, 2024, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file include/sslopt-longopts.h
*/

{"ssl-mode",
 OPT_SSL_MODE,
 "SSL connection mode.",
 nullptr,
 nullptr,
 nullptr,
 GET_STR,
 REQUIRED_ARG,
 0,
 0,
 0,
 nullptr,
 0,
 nullptr},
    {"ssl-ca",
     OPT_SSL_CA,
     "CA file in PEM format.",
     &opt_ssl_ca,
     &opt_ssl_ca,
     nullptr,
     GET_STR,
     REQUIRED_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},
    {"ssl-capath",
     OPT_SSL_CAPATH,
     "CA directory.",
     &opt_ssl_capath,
     &opt_ssl_capath,
     nullptr,
     GET_STR,
     REQUIRED_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},
    {"ssl-cert",
     OPT_SSL_CERT,
     "X509 cert in PEM format.",
     &opt_ssl_cert,
     &opt_ssl_cert,
     nullptr,
     GET_STR,
     REQUIRED_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},
    {"ssl-cipher",
     OPT_SSL_CIPHER,
     "SSL cipher to use.",
     &opt_ssl_cipher,
     &opt_ssl_cipher,
     nullptr,
     GET_STR,
     REQUIRED_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},
    {"ssl-key",
     OPT_SSL_KEY,
     "X509 key in PEM format.",
     &opt_ssl_key,
     &opt_ssl_key,
     nullptr,
     GET_STR,
     REQUIRED_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},
    {"ssl-crl",
     OPT_SSL_CRL,
     "Certificate revocation list.",
     &opt_ssl_crl,
     &opt_ssl_crl,
     nullptr,
     GET_STR,
     REQUIRED_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},
    {"ssl-crlpath",
     OPT_SSL_CRLPATH,
     "Certificate revocation list path.",
     &opt_ssl_crlpath,
     &opt_ssl_crlpath,
     nullptr,
     GET_STR,
     REQUIRED_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},
    {"tls-version",
     OPT_TLS_VERSION,
     "TLS version to use, "
#ifdef HAVE_TLSv13
     "permitted values are: TLSv1.2, TLSv1.3",
#else
     "permitted values are: TLSv1.2",
#endif
     &opt_tls_version,
     &opt_tls_version,
     nullptr,
     GET_STR,
     REQUIRED_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},
    {"ssl-fips-mode",
     OPT_SSL_FIPS_MODE,
     "SSL FIPS mode (applies only for OpenSSL); "
     "permitted values are: OFF, ON, STRICT",
     nullptr,
     nullptr,
     nullptr,
     GET_STR,
     REQUIRED_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},
    {"tls-ciphersuites",
     OPT_TLS_CIPHERSUITES,
     "TLS v1.3 cipher to use.",
     &opt_tls_ciphersuites,
     &opt_tls_ciphersuites,
     nullptr,
     GET_STR,
     REQUIRED_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},
    {"ssl-session-data",
     OPT_SSL_SESSION_DATA,
     "Session data file to use to enable ssl session reuse",
     &opt_ssl_session_data,
     &opt_ssl_session_data,
     nullptr,
     GET_STR,
     REQUIRED_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},
    {"ssl-session-data-continue-on-failed-reuse",
     OPT_SSL_SESSION_DATA_CONTINUE_ON_FAILED_REUSE,
     "If set to ON, this option will allow connection to succeed even if "
     "session data cannot be reused.",
     &opt_ssl_session_data_continue_on_failed_reuse,
     &opt_ssl_session_data_continue_on_failed_reuse,
     nullptr,
     GET_BOOL,
     OPT_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},
    {"tls-sni-servername",
     OPT_TLS_SNI_SERVERNAME,
     "The SNI server name to pass to server",
     &opt_tls_sni_servername,
     &opt_tls_sni_servername,
     nullptr,
     GET_STR,
     REQUIRED_ARG,
     0,
     0,
     0,
     nullptr,
     0,
     nullptr},