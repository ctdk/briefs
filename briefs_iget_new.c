/* briefs_iget - get an inode by number */
struct inode *briefs_iget(struct super_block *sb, u64 ino) {
	struct inode *inode;
	struct briefs_inode_info *binfo;
	struct buffer_head *bh;
	struct briefs_inode *disk_inode;
	struct briefs_sb_info *bsi;
	u64 inodeTableBlock;
	u64 inodeBlock;
	u64 inodeOffset;

	pr_debug("briefs: iget inode %llu\n", ino);

	bsi = sb->s_fs_info;
	if (!bsi || !bsi->sb) {
		pr_err("briefs: no sb_info for ino %llu\n", ino);
		return ERR_PTR(-EIO);
	}

	/* Calculate inode location: inode table follows data bitmap */
	inodeTableBlock = bsi->sb->data_bitmap_offset + bsi->sb->data_bitmap_blocks;
	/* Inode table starts at inodeTableBlock. Each block holds 8 inodes (512 bytes each).
	 * inodeIndex is the 0-based index into the inode table.
	 * inodeBlock is the block offset within the inode table.
	 * inodeOffset is the byte offset within that block.
	 */
	u64 inodeIndex = ino - 1;
	inodeBlock = inodeIndex / (sb->s_blocksize / BRIEFS_INODE_SIZE);
	inodeOffset = (inodeIndex % (sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;

	inode = iget_locked(sb, ino);
	if (!inode) {
		pr_err("briefs: iget_locked failed\n");
		return ERR_PTR(-ENOMEM);
	}

	if (inode->i_state & I_NEW) {
		/* Read inode from disk */
		bh = sb_bread(sb, inodeTableBlock + inodeBlock);
		if (!bh) {
			pr_err("briefs: unable to read inode block for ino %llu\n", ino);
			unlock_new_inode(inode);
			iput(inode);
			return ERR_PTR(-EIO);
		}

		disk_inode = (struct briefs_inode *)(bh->b_data + inodeOffset);

		if (disk_inode->magic != 0x494E4F44) {
			pr_err("briefs: invalid inode magic for ino %llu: 0x%08x\n", ino, disk_inode->magic);
			brelse(bh);
			unlock_new_inode(inode);
			iput(inode);
			return ERR_PTR(-EINVAL);
		}

		/* Copy disk inode to VFS inode */
		binfo = (struct briefs_inode_info *)inode;
		memcpy(&binfo->disk_inode, disk_inode, sizeof(struct briefs_inode));
		binfo->inode_number = ino;

		/* Set VFS inode fields from disk inode */
		inode->i_mode = disk_inode->filemode;
		inode->i_uid = make_kuid(&init_user_ns, disk_inode->uid);
		inode->i_gid = make_kgid(&init_user_ns, disk_inode->gid);
		inode->i_size = disk_inode->filesize;
		inode->i_blocks = 0;  /* We'll compute this from extents later */
		inode->i_nlink = disk_inode->nlinks;

		brelse(bh);
		unlock_new_inode(inode);
	}

	return inode;
}
