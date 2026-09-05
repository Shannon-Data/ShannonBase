# Compare the captured boundary-case answers of one Rapid run against the
# InnoDB baseline. $rpd names the database holding the Rapid answers.

--eval SELECT (SELECT COUNT(*) FROM sh_wf_base.e1) - (SELECT COUNT(*) FROM $rpd.e1) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.e1 EXCEPT ALL SELECT * FROM $rpd.e1) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.e1 EXCEPT ALL SELECT * FROM sh_wf_base.e1) y) AS only_rapid
--eval SELECT (SELECT COUNT(*) FROM sh_wf_base.e2) - (SELECT COUNT(*) FROM $rpd.e2) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.e2 EXCEPT ALL SELECT * FROM $rpd.e2) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.e2 EXCEPT ALL SELECT * FROM sh_wf_base.e2) y) AS only_rapid
--eval SELECT (SELECT COUNT(*) FROM sh_wf_base.e3) - (SELECT COUNT(*) FROM $rpd.e3) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.e3 EXCEPT ALL SELECT * FROM $rpd.e3) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.e3 EXCEPT ALL SELECT * FROM sh_wf_base.e3) y) AS only_rapid
--eval SELECT (SELECT COUNT(*) FROM sh_wf_base.e4) - (SELECT COUNT(*) FROM $rpd.e4) AS row_delta, (SELECT COUNT(*) FROM (SELECT * FROM sh_wf_base.e4 EXCEPT ALL SELECT * FROM $rpd.e4) x) AS only_innodb, (SELECT COUNT(*) FROM (SELECT * FROM $rpd.e4 EXCEPT ALL SELECT * FROM sh_wf_base.e4) y) AS only_rapid
