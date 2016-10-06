#ifndef _JIN_H
#define _JIN_H

#define JIN (1)


struct hstorage_conf {
	sector_t start_sect_of_data;
	struct md_rdev * meta_dev;
	struct md_rdev * data_dev;
};

#endif
