# Compare the captured answers of one Rapid run against the InnoDB baseline.
# $rpd names the database holding the Rapid answers. Runs on the primary
# engine: none of these tables are loaded into Rapid.
#
# base_rows/rapid_rows catch a lost or duplicated row, the two EXCEPT ALL
# counts catch a wrong value; all four must be zero-difference.

--eval SELECT (SELECT COUNT(*) FROM sh_wf_base.a1) - (SELECT COUNT(*) FROM $rpd.a1) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.a1 EXCEPT ALL SELECT * FROM $rpd.a1) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.a1 EXCEPT ALL SELECT * FROM sh_wf_base.a1) y) AS only_rapid
--eval SELECT (SELECT COUNT(*) FROM sh_wf_base.a2) - (SELECT COUNT(*) FROM $rpd.a2) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.a2 EXCEPT ALL SELECT * FROM $rpd.a2) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.a2 EXCEPT ALL SELECT * FROM sh_wf_base.a2) y) AS only_rapid
--eval SELECT (SELECT COUNT(*) FROM sh_wf_base.a3) - (SELECT COUNT(*) FROM $rpd.a3) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.a3 EXCEPT ALL SELECT * FROM $rpd.a3) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.a3 EXCEPT ALL SELECT * FROM sh_wf_base.a3) y) AS only_rapid
--eval SELECT (SELECT COUNT(*) FROM sh_wf_base.a4) - (SELECT COUNT(*) FROM $rpd.a4) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.a4 EXCEPT ALL SELECT * FROM $rpd.a4) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.a4 EXCEPT ALL SELECT * FROM sh_wf_base.a4) y) AS only_rapid
