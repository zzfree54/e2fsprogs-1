/*
 * pass2.c --- check directory structure
 * 
 * Copyright (C) 1993, 1994 Theodore Ts'o.  This file may be
 * redistributed under the terms of the GNU Public License.
 * 
 * Pass 2 of e2fsck iterates through all active directory inodes, and
 * applies to following tests to each directory entry in the directory
 * blocks in the inodes:
 *
 *	- The length of the directory entry (rec_len) should be at
 * 		least 8 bytes, and no more than the remaining space
 * 		left in the directory block.
 * 	- The length of the name in the directory entry (name_len)
 * 		should be less than (rec_len - 8).  
 *	- The inode number in the directory entry should be within
 * 		legal bounds.
 * 	- The inode number should refer to a in-use inode.
 *	- The first entry should be '.', and its inode should be
 * 		the inode of the directory.
 * 	- The second entry should be '..'.
 *
 * To minimize disk seek time, the directory blocks are processed in
 * sorted order of block numbers.
 *
 * Pass 2 also collects the following information:
 * 	- The inode numbers of the subdirectories for each directory.
 *
 * Pass 2 relies on the following information from previous passes:
 * 	- The directory information collected in pass 1.
 * 	- The inode_used_map bitmap
 * 	- The inode_bad_map bitmap
 * 	- The inode_dir_map bitmap
 *
 * Pass 2 frees the following data structures
 * 	- The inode_bad_map bitmap
 */

#include "et/com_err.h"

#include "e2fsck.h"

/*
 * Keeps track of how many times an inode is referenced.
 */
unsigned short * inode_count;

static void deallocate_inode(ext2_filsys fs, ino_t ino,
			     char* block_buf);
static int process_bad_inode(ext2_filsys fs, ino_t dir, ino_t ino);
static int check_dir_block(ext2_filsys fs,
			   struct dir_block_struct *dir_blocks_info,
			   char *buf);
static int allocate_dir_block(ext2_filsys fs,
			      struct dir_block_struct *dir_blocks_info,
			      char *buf);
static int update_dir_block(ext2_filsys fs,
			    blk_t	*block_nr,
			    int blockcnt,
			    void *private);

void pass2(ext2_filsys fs)
{
	int	i;
	char	*buf;
	struct resource_track	rtrack;
	
	init_resource_track(&rtrack);

#ifdef MTRACE
	mtrace_print("Pass 2");
#endif

	if (!preen)
		printf("Pass 2: Checking directory structure\n");
	inode_count = allocate_memory((fs->super->s_inodes_count + 1) *
				      sizeof(unsigned short),
				      "buffer for inode count");

	buf = allocate_memory(fs->blocksize, "directory scan buffer");

	for (i=0; i < dir_block_count; i++)
		check_dir_block(fs, &dir_blocks[i], buf);
	     
	free(buf);
	free(dir_blocks);
	if (inode_bad_map) {
		ext2fs_free_inode_bitmap(inode_bad_map);
		inode_bad_map = 0;
	}
	if (tflag > 1) {
		printf("Pass 2: ");
		print_resource_track(&rtrack);
	}
}

/*
 * Make sure the first entry in the directory is '.', and that the
 * directory entry is sane.
 */
static int check_dot(ext2_filsys fs,
		     struct ext2_dir_entry *dirent,
		     ino_t ino)
{
	struct ext2_dir_entry *nextdir;
	int	status = 0;
	int	created = 0;
	int	new_len;
	const char 	*question = 0;
	
	if (!dirent->inode) {
		printf("Missing '.' in directory inode %lu.\n", ino);
		question = "Fix";
	} else if ((dirent->name_len != 1) ||
		   strncmp(dirent->name, ".", dirent->name_len)) {
		char *name = malloc(dirent->name_len + 1);
		if (!name)
			fatal_error("Couldn't allocate . name");
		strncpy(name, dirent->name, dirent->name_len);
		name[dirent->name_len] = '\0';
		printf("First entry in directory inode %lu contains '%s' "
		       "(inode=%u)\n", ino, name, dirent->inode);
		printf("instead of '.'.\n");
		free(name);
		question = "Change to be '.'";
	}
	if (question) {
		if (dirent->rec_len < 12)
			fatal_error("Cannot fix, insufficient space to add '.'");
		preenhalt(fs);
		if (ask(question, 1)) {
			dirent->inode = ino;
			dirent->name_len = 1;
			dirent->name[0] = '.';
			status = 1;
			created = 1;
		} else {
			ext2fs_unmark_valid(fs);
			return 0;
		}
	}
	if (dirent->inode != ino) {
		printf("Bad inode number for '.' in directory inode %lu.\n",
		       ino);
		preenhalt(fs);
		if (ask("Fix", 1)) {
			dirent->inode = ino;
			status = 1;
		} else
			ext2fs_unmark_valid(fs);
	}
	if (dirent->rec_len > 12) {
		new_len = dirent->rec_len - 12;
		if (new_len > 12) {
			preenhalt(fs);
			if (created ||
			    ask("Directory entry for '.' is big.  Split", 1)) {
				nextdir = (struct ext2_dir_entry *)
					((char *) dirent + 12);
				dirent->rec_len = 12;
				nextdir->rec_len = new_len;
				nextdir->inode = 0;
				nextdir->name_len = 0;
				status = 1;
			}
		}
	}
	return status;
}

/*
 * Make sure the second entry in the directory is '..', and that the
 * directory entry is sane.  We do not check the inode number of '..'
 * here; this gets done in pass 3.
 */
static int check_dotdot(ext2_filsys fs,
			struct ext2_dir_entry *dirent,
			struct dir_info *dir)
{
	ino_t	ino = dir->ino;
	const char	*question = 0;
	
	if (!dirent->inode) {
		printf("Missing '..' in directory inode %lu.\n", ino);
		question = "Fix";
	} else if ((dirent->name_len != 2) ||
	    strncmp(dirent->name, "..", dirent->name_len)) {
		char *name = malloc(dirent->name_len + 1);
		if (!name)
			fatal_error("Couldn't allocate bad .. name");
		strncpy(name, dirent->name, dirent->name_len);
		name[dirent->name_len] = '\0';
		printf("Second entry in directory inode %lu contains '%s' "
		       "(inode=%u)\n", ino, name, dirent->inode);
		printf("instead of '..'.\n");
		free(name);
		question = "Change to be '..'";
	}
	if (question) {
		if (dirent->rec_len < 12)
			fatal_error("Cannot fix, insufficient space to add '..'");
		preenhalt(fs);
		if (ask(question, 1)) {
			/*
			 * Note: we don't have the parent inode just
			 * yet, so we will fill it in with the root
			 * inode.  This will get fixed in pass 3.
			 */
			dirent->inode = EXT2_ROOT_INO;
			dirent->name_len = 2;
			dirent->name[0] = '.';
			dirent->name[1] = '.';
			return 1;
		} else
			ext2fs_unmark_valid(fs);
		return 0;
	}
	dir->dotdot = dirent->inode;
	return 0;
}

static char unknown_pathname[] = "???";

/*
 * Check to make sure a directory entry doesn't contain any illegal
 * characters.
 */
static int check_name(ext2_filsys fs,
		      struct ext2_dir_entry *dirent,
		      ino_t dir_ino,
		      char *name)
{
	int	i;
	int	fixup = -1;
	char	*pathname;
	int	ret = 0;
	errcode_t	retval;
	
	for ( i = 0; i < dirent->name_len; i++) {
		if (dirent->name[i] == '/' || dirent->name[i] == '\0') {
			if (fixup < 0) {
				retval = ext2fs_get_pathname(fs, dir_ino,
							     0, &pathname);
				if (retval) {
					com_err(program_name, retval, "while getting pathname in check_name");
					pathname = unknown_pathname;
				}
				printf ("Bad file name '%s' (contains '/' or "
					" null) in directory '%s' (%lu)\n",
					name, pathname, dir_ino);
				if (pathname != unknown_pathname)
					free(pathname);
				preenhalt(fs);
				fixup = ask("Replace '/' or null by '.'", 1);
			}
			if (fixup) {
				dirent->name[i] = '.';
				ret = 1;
			} else
				ext2fs_unmark_valid(fs);
		}
	}
	return ret;
}

static int check_dir_block(ext2_filsys fs,
			   struct dir_block_struct *db,
			   char *buf)
{
	struct dir_info		*subdir, *dir;
	struct ext2_dir_entry 	*dirent;
	int			offset = 0;
	int			dir_modified = 0;
	errcode_t		retval;
	char			*path1, *path2;
	int			dot_state, name_len;
	blk_t			block_nr = db->blk;
	ino_t 			ino = db->ino;
	char			name[EXT2_NAME_LEN+1];

	/*
	 * Make sure the inode is still in use (could have been 
	 * deleted in the duplicate/bad blocks pass.
	 */
	if (!(ext2fs_test_inode_bitmap(inode_used_map, ino))) 
		return 0;

	if (db->blk == 0) {
		if (allocate_dir_block(fs, db, buf))
			return 0;
		block_nr = db->blk;
	}
	
	if (db->blockcnt)
		dot_state = 2;
	else
		dot_state = 0;

#if 0
	printf("In process_dir_block block %lu, #%d, inode %lu\n", block_nr,
	       db->blockcnt, ino);
#endif
	
	retval = ext2fs_read_dir_block(fs, block_nr, buf);
	if (retval) {
		com_err(program_name, retval,
			"while reading directory block %d", block_nr);
	}

	do {
		dot_state++;
		dirent = (struct ext2_dir_entry *) (buf + offset);
		if (((offset + dirent->rec_len) > fs->blocksize) ||
		    (dirent->rec_len < 8) ||
		    ((dirent->name_len+8) > dirent->rec_len)) {
			printf("Directory inode %lu, block %d, offset %d: directory corrupted\n",
			       ino, db->blockcnt, offset);
			preenhalt(fs);
			if (ask("Salvage", 1)) {
				dirent->rec_len = fs->blocksize - offset;
				dirent->name_len = 0;
				dirent->inode = 0;
				dir_modified++;
			} else {
				ext2fs_unmark_valid(fs);
				return DIRENT_ABORT;
			}
		}

		name_len = dirent->name_len;
		if (dirent->name_len > EXT2_NAME_LEN) {
			printf("Directory inode %lu, block %d, offset %d: filename too long\n",
			       ino, db->blockcnt, offset);
			preenhalt(fs);
			if (ask("Truncate filename", 1)) {
				dirent->name_len = EXT2_NAME_LEN;
				dir_modified++;
			}
			name_len = EXT2_NAME_LEN;
		}

		strncpy(name, dirent->name, name_len);
		name[name_len] = '\0';

		if (dot_state == 1) {
			if (check_dot(fs, dirent, ino))
				dir_modified++;
		} else if (dot_state == 2) {
			dir = get_dir_info(ino);
			if (!dir) {
				printf("Internal error: couldn't find dir_info for %lu\n",
				       ino);
				fatal_error(0);
			}
			if (check_dotdot(fs, dirent, dir))
				dir_modified++;
		} else if (dirent->inode == ino) {
			retval = ext2fs_get_pathname(fs, ino, 0, &path1);
			if (retval)
				path1 = unknown_pathname;
			printf("Entry '%s' in %s (%lu) is a link to '.'  ",
			       name, path1, ino);
			if (path1 != unknown_pathname)
				free(path1);
			preenhalt(fs);
			if (ask("Clear", 1)) {
				dirent->inode = 0;
				dir_modified++;
			}
		}
		if (!dirent->inode) 
			goto next;
		
#if 0
		printf("Entry '%s', name_len %d, rec_len %d, inode %lu... ",
		       name, dirent->name_len, dirent->rec_len, dirent->inode);
#endif
		if (check_name(fs, dirent, ino, name))
			dir_modified++;

		/*
		 * Make sure the inode listed is a legal one.
		 */ 
		if (((dirent->inode != EXT2_ROOT_INO) &&
		     (dirent->inode < EXT2_FIRST_INODE(fs->super))) ||
		    (dirent->inode > fs->super->s_inodes_count)) {
			retval = ext2fs_get_pathname(fs, ino, 0, &path1);
			if (retval)
				path1 = unknown_pathname;
			printf("Entry '%s' in %s (%lu) has bad inode #: %u.\n",
			       name, path1, ino, dirent->inode);
			if (path1 != unknown_pathname)
				free(path1);
			preenhalt(fs);
			if (ask("Clear", 1)) {
				dirent->inode = 0;
				dir_modified++;
				goto next;
			} else
				ext2fs_unmark_valid(fs);
		}

		/*
		 * If the inode is unusued, offer to clear it.
		 */
		if (!(ext2fs_test_inode_bitmap(inode_used_map,
					       dirent->inode))) {
			retval = ext2fs_get_pathname(fs, ino, 0, &path1);
			if (retval)
				path1 = unknown_pathname;
			printf("Entry '%s' in %s (%lu) has deleted/unused inode %u.\n",
			       name, path1, ino, dirent->inode);
			if (path1 != unknown_pathname)
				free(path1);
			if (ask("Clear", 1)) {
				dirent->inode = 0;
				dir_modified++;
				goto next;
			} else
				ext2fs_unmark_valid(fs);
		}

		/*
		 * If the inode was marked as having bad fields in
		 * pass1, process it and offer to fix/clear it.
		 * (We wait until now so that we can display the
		 * pathname to the user.)
		 */
		if (inode_bad_map &&
		    ext2fs_test_inode_bitmap(inode_bad_map,
					     dirent->inode)) {
			if (process_bad_inode(fs, ino, dirent->inode)) {
				dirent->inode = 0;
				dir_modified++;
				goto next;
			}
		}

		/*
		 * If this is a directory, then mark its parent in its
		 * dir_info structure.  If the parent field is already
		 * filled in, then this directory has more than one
		 * hard link.  We assume the first link is correct,
		 * and ask the user if he/she wants to clear this one.
		 */
		if ((dot_state > 2) &&
		    (ext2fs_test_inode_bitmap(inode_dir_map,
					      dirent->inode))) {
			subdir = get_dir_info(dirent->inode);
			if (!subdir) {
				printf("INTERNAL ERROR: missing dir %u\n",
				       dirent->inode);
				fatal_error(0);
			}
			if (subdir->parent) {
				retval = ext2fs_get_pathname(fs, ino,
							     0, &path1);
				if (retval)
					path1 = unknown_pathname;
				retval = ext2fs_get_pathname(fs,
							     subdir->parent,
							     dirent->inode,
							     &path2);
				if (retval)
					path2 = unknown_pathname;
				printf("Entry '%s' in %s (%lu) is a link to directory %s (%u).\n",
				       name, path1, ino, path2, 
				       dirent->inode);
				if (path1 != unknown_pathname)
					free(path1);
				if (path2 != unknown_pathname)
					free(path2);
				if (ask("Clear", 1)) {
					dirent->inode = 0;
					dir_modified++;
					goto next;
				} else
					ext2fs_unmark_valid(fs);
			}
			subdir->parent = ino;
		}
		
		if (inode_count[dirent->inode]++ > 0)
			fs_links_count++;
		fs_total_count++;
	next:
		offset += dirent->rec_len;
	} while (offset < fs->blocksize);
#if 0
	printf("\n");
#endif
	if (offset != fs->blocksize) {
		printf("Final rec_len is %d, should be %d\n",
		       dirent->rec_len,
		       dirent->rec_len - fs->blocksize + offset);
	}
	if (dir_modified) {
		retval = ext2fs_write_dir_block(fs, block_nr, buf);
		if (retval) {
			com_err(program_name, retval,
				"while writing directory block %d", block_nr);
		}
		ext2fs_mark_changed(fs);
	}
	return 0;
}

/*
 * This function is called to deallocate a block, and is an interator
 * functioned called by deallocate inode via ext2fs_iterate_block().
 */
static int deallocate_inode_block(ext2_filsys fs,
			     blk_t	*block_nr,
			     int blockcnt,
			     void *private)
{
	if (!*block_nr)
		return 0;
	ext2fs_unmark_block_bitmap(block_found_map, *block_nr);
	ext2fs_unmark_block_bitmap(fs->block_map, *block_nr);
	return 0;
}
		
/*
 * This fuction deallocates an inode
 */
static void deallocate_inode(ext2_filsys fs, ino_t ino,
			     char* block_buf)
{
	errcode_t		retval;
	struct ext2_inode	inode;

	inode_link_info[ino] = 0;
	e2fsck_read_inode(fs, ino, &inode, "deallocate_inode");
	inode.i_links_count = 0;
	inode.i_dtime = time(0);
	e2fsck_write_inode(fs, ino, &inode, "deallocate_inode");

	/*
	 * Fix up the bitmaps...
	 */
	read_bitmaps(fs);
	ext2fs_unmark_inode_bitmap(inode_used_map, ino);
	ext2fs_unmark_inode_bitmap(inode_dir_map, ino);
	if (inode_bad_map)
		ext2fs_unmark_inode_bitmap(inode_bad_map, ino);
	ext2fs_unmark_inode_bitmap(fs->inode_map, ino);
	ext2fs_mark_ib_dirty(fs);

	if (!inode_has_valid_blocks(&inode))
		return;
	
	ext2fs_mark_bb_dirty(fs);
	retval = ext2fs_block_iterate(fs, ino, 0, block_buf,
				      deallocate_inode_block, 0);
	if (retval)
		com_err("deallocate_inode", retval,
			"while calling ext2fs_block_iterate for inode %d",
			ino);
}

/*
 * These two subroutines are used by process_bad_inode; it is used to
 * make sure that certain reserved fields are really zero.  If not,
 * prompt the user if he/she wants us to zeroize them.
 */
static void check_for_zero_u32(ext2_filsys fs, ino_t ino, char *pathname,
				const char *name, __u32 *val,
				int *modified)
{
	char prompt[80];
	
	if (*val) {
		printf("%s for inode %lu (%s) is %u, should be zero.\n",
		       name, ino, pathname, *val);
		preenhalt(fs);
		sprintf(prompt, "Clear %s", name);
		if (ask(prompt, 1)) {
			*val = 0;
			*modified = 1;
		} else
			ext2fs_unmark_valid(fs);
	}
}

static void check_for_zero_u8(ext2_filsys fs, ino_t ino, char *pathname,
				const char *name, __u8 *val,
				int *modified)
{
	char prompt[80];
	
	if (*val) {
		printf("%s for inode %lu (%s) is %d, should be zero.\n",
		       name, ino, pathname, *val);
		preenhalt(fs);
		sprintf(prompt, "Clear %s", name);
		if (ask(prompt, 1)) {
			*val = 0;
			*modified = 1;
		} else
			ext2fs_unmark_valid(fs);
	}
}

	

static int process_bad_inode(ext2_filsys fs, ino_t dir, ino_t ino)
{
	struct ext2_inode	inode;
	errcode_t		retval;
	int			inode_modified = 0;
	char			*pathname;
	unsigned char		*frag, *fsize;

	e2fsck_read_inode(fs, ino, &inode, "process_bad_inode");
	retval = ext2fs_get_pathname(fs, dir, ino, &pathname);
	if (retval) {
		com_err("process_bad_inode", retval,
			"while getting pathname for inode %d",
			ino);
		return 0;
	}
	if (!LINUX_S_ISDIR(inode.i_mode) && !LINUX_S_ISREG(inode.i_mode) &&
	    !LINUX_S_ISCHR(inode.i_mode) && !LINUX_S_ISBLK(inode.i_mode) &&
	    !LINUX_S_ISLNK(inode.i_mode) && !LINUX_S_ISFIFO(inode.i_mode) &&
	    !(LINUX_S_ISSOCK(inode.i_mode))) {
		printf("Inode %lu (%s) has a bad mode (0%o).\n",
		       ino, pathname, inode.i_mode);
		preenhalt(fs);
		if (ask("Clear", 1)) {
			deallocate_inode(fs, ino, 0);
			free(pathname);
			return 1;
		} else
			ext2fs_unmark_valid(fs);
	}
	check_for_zero_u32(fs, ino, pathname, "i_faddr", &inode.i_faddr,
			    &inode_modified);

	switch (fs->super->s_creator_os) {
	    case EXT2_OS_LINUX:
		frag = &inode.osd2.linux2.l_i_frag;
		fsize = &inode.osd2.linux2.l_i_fsize;
		break;
	    case EXT2_OS_HURD:
		frag = &inode.osd2.hurd2.h_i_frag;
		fsize = &inode.osd2.hurd2.h_i_fsize;
		break;
	    case EXT2_OS_MASIX:
		frag = &inode.osd2.masix2.m_i_frag;
		fsize = &inode.osd2.masix2.m_i_fsize;
		break;
	    default:
		frag = fsize = 0;
	}
	if (frag)
		check_for_zero_u8(fs, ino, pathname, "i_frag", frag,
				  &inode_modified);
	if (fsize)
		check_for_zero_u8(fs, ino, pathname, "i_fsize", fsize,
				  &inode_modified);

	check_for_zero_u32(fs, ino, pathname, "i_file_acl", &inode.i_file_acl,
			    &inode_modified);
	check_for_zero_u32(fs, ino, pathname, "i_dir_acl", &inode.i_dir_acl,
			    &inode_modified);
	free(pathname);
	if (inode_modified)
		e2fsck_write_inode(fs, ino, &inode, "process_bad_inode");
	return 0;
}


/*
 * allocate_dir_block --- this function allocates a new directory
 * 	block for a particular inode; this is done if a directory has
 * 	a "hole" in it, or if a directory has a illegal block number
 * 	that was zeroed out and now needs to be replaced.
 */
static int allocate_dir_block(ext2_filsys fs,
			      struct dir_block_struct *db,
			      char *buf)
{
	blk_t			blk;
	char			*block;
	struct ext2_inode	inode;
	errcode_t		retval;

	printf("Directory inode %lu has a hole at block #%d\n",
	       db->ino, db->blockcnt);
	if (ask("Allocate block", 1) == 0)
		return 1;

	/*
	 * Read the inode and block bitmaps in; we'll be messing with
	 * them.
	 */
	read_bitmaps(fs);
	
	/*
	 * First, find a free block
	 */
	retval = ext2fs_new_block(fs, 0, block_found_map, &blk);
	if (retval) {
		com_err("allocate_dir_block", retval,
			"while trying to fill a hole in a directory inode");
		return 1;
	}
	ext2fs_mark_block_bitmap(block_found_map, blk);
	ext2fs_mark_block_bitmap(fs->block_map, blk);
	ext2fs_mark_bb_dirty(fs);

	/*
	 * Now let's create the actual data block for the inode
	 */
	if (db->blockcnt)
		retval = ext2fs_new_dir_block(fs, 0, 0, &block);
	else
		retval = ext2fs_new_dir_block(fs, db->ino, EXT2_ROOT_INO,
					      &block);

	if (retval) {
		com_err("allocate_dir_block", retval,
			"while creating new directory block");
		return 1;
	}

	retval = ext2fs_write_dir_block(fs, blk, block);
	free(block);
	if (retval) {
		com_err("allocate_dir_block", retval,
			"while writing an empty directory block");
		return 1;
	}

	/*
	 * Update the inode block count
	 */
	e2fsck_read_inode(fs, db->ino, &inode, "allocate_dir_block");
	inode.i_blocks += fs->blocksize / 512;
	if (inode.i_size < (db->blockcnt+1) * fs->blocksize)
		inode.i_size = (db->blockcnt+1) * fs->blocksize;
	e2fsck_write_inode(fs, db->ino, &inode, "allocate_dir_block");

	/*
	 * Finally, update the block pointers for the inode
	 */
	db->blk = blk;
	retval = ext2fs_block_iterate(fs, db->ino, BLOCK_FLAG_HOLE,
				      0, update_dir_block, db);
	if (retval) {
		com_err("allocate_dir_block", retval,
			"while calling ext2fs_block_iterate");
		return 1;
	}

	return 0;
}

/*
 * This is a helper function for allocate_dir_block().
 */
static int update_dir_block(ext2_filsys fs,
			    blk_t	*block_nr,
			    int blockcnt,
			    void *private)
{
	struct dir_block_struct *db = private;

	if (db->blockcnt == blockcnt) {
		*block_nr = db->blk;
		return BLOCK_CHANGED;
	}
	return 0;
}
	
