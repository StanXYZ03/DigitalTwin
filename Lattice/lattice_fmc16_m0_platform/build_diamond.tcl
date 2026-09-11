prj_project open "X024000HC_FMC16_LINK_TEST.ldf"
prj_run Export -impl impl_link_test -task Jedecgen
prj_project save
prj_project close
exit
