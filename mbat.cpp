/*
 * GCTA: a tool for Genome-wide Complex Trait Analysis
 *
 * Implementation of gene-based association test (GBAT) in GCTA
 *
 * 2013 by Jian Yang <jian.yang@uq.edu.au>
 *
 * This file is distributed under the GNU General Public
 * License, Version 2.  Please see the file COPYING for more
 * details
 */

#include "gcta.h"

/*
 * Iterative VIF (Variance Inflation Factor) method to reduce collinearity.
 * Returns position of snp with the highest VIF in correlation matrix (to remove)
 * Returns -1 if no snps to remove with VIF greater than threshold (10)
 * Continue calling this function, and reducing correlation matrix until -1 returned
 */

int gcta::sbat_VIF_iter_rm_colin(MatrixXf R)
{
        SelfAdjointEigenSolver<MatrixXf> pca(R.array());
        int j, n, k;
        int size = R.col(0).size();
        int threshold = 10; //VIF threshold to remove snp

        VectorXf d_i = pca.eigenvalues(); //degree freedom LD matrix
        double eff_m = 0;
        for(j = 0; j < size; j ++){
        	if(d_i(j) < 1e-5) d_i(j) = 0.0;
        	else{
        		d_i(j) = 1.0 / d_i(j);
        		eff_m++;
        	}
        }

        MatrixXf R_i = pca.eigenvectors() * d_i.asDiagonal() * pca.eigenvectors().transpose();
        R_i = R_i.array();

       VectorXf Q_diag(size);
        for(j = 0; j < size; j ++) Q_diag(j) = R_i.col(j).dot(R.row(j).transpose());
        VectorXf multi_rsq_buf(size);
    	for(j = 0; j < size; j ++){
    		if(fabs(Q_diag[j] - 1.0) < 0.01) multi_rsq_buf[j] = 1.0 - 1.0 / R_i(j,j);
    		else multi_rsq_buf[j] = 1.0;
            if(multi_rsq_buf[j] > 1.0) multi_rsq_buf[j] = 1.0;
    	}
        VectorXf multi_rsq_buf_adj = multi_rsq_buf.array() - (1.0 - multi_rsq_buf.array()) * (eff_m / ((double)n - eff_m - 1.0)); //ignore adjust for now

        R.diagonal() = VectorXf::Zero(size);
        R = R.array() * R.array();
        VectorXf max_rsq_buf(size);
        vector<int> max_pos_buf(size);
        VectorXf::Index max_pos_pnt;
        for(j = 0; j < size; j++){
            R.col(j).maxCoeff(&max_pos_pnt);
            max_pos_buf[j] = (int)max_pos_pnt;
            max_rsq_buf[j] = R.col(j)[max_pos_buf[j]];
            if(multi_rsq_buf[j] < max_rsq_buf[j]) multi_rsq_buf[j] = max_rsq_buf[j];
        }

       //Ignoring break point
        /*
        for (j = 0, k = brk_pnt[i]; j < size; j++, k++) {
            if(multi_rsq_adj[k] <= multi_rsq_buf_adj[j]){
            	multi_rsq[k] = multi_rsq_buf[j];
            	multi_rsq_adj[k] = multi_rsq_buf_adj[j];
            }
            if(max_rsq[k] < max_rsq_buf[j]){
                max_rsq[k] = max_rsq_buf[j];
                max_pos[k] = max_pos_buf[j] + brk_pnt[i];
            }
        }
        */

        VectorXf vif(size);
        float max = 0;
        int pos = -1;

        for (j = 0 ; j < size ; j++ )
        {
            if (fabs(1 - multi_rsq_buf[j]) < 1e-5){
                vif[j] = 1e8;
            }
            else {
                vif[j] = fabs(1/(1-multi_rsq_buf[j]));
            }
            if (vif[j] > max && vif[j] > threshold) {
                max = vif[j];
                pos = j;
            }
       }

        return pos;
}

void gcta::sbat_multi_calcu_V(vector<int> &snp_indx, eigenVector set_beta, eigenVector set_se, double &Vscore, double &Vscore_p, int &snp_count, vector<string> &snp_kept)
{
    int i = 0, j = 0, k = 0, n = _keep.size(), m = snp_indx.size();
    VectorXd eigenval;

    MatrixXf X;
    MatrixXd SE;
    MatrixXd V;

    make_XMat_subset(X, snp_indx, false);

    VectorXd sumsq_x(m);
    for (j = 0; j < m; j++) sumsq_x[j] = X.col(j).dot(X.col(j));

    MatrixXf C = X.transpose() * X;
    X.resize(0,0);
    #pragma omp parallel for private(j)
    for (i = 0; i < m; i++) {
        for (j = 0; j < m; j++) {
            double d_buf = sqrt(sumsq_x[i] * sumsq_x[j]);
            if(d_buf>0.0) C(i,j) /= d_buf;
            else C(i,j) = 0.0;
        }
    }

    /* DEBUG
    string pgoodsnpfile = _out + ".presnps";
    ofstream pogoodsnp(pgoodsnpfile.c_str());
    pogoodsnp << "snp beta se" << endl;
    for (i = 0; i < snp_kept.size(); i++) pogoodsnp << snp_kept[i] << " " << set_beta[i] << " " << set_se[i] << endl;
    pogoodsnp.close();
    */

    eigenVector snp_beta = set_beta;
    eigenVector snp_btse = set_se;
    vector<string> snp_keep = snp_kept;

    //Because resize is destructive, write values to tmp vector, resize, and assign from temp vector
    eigenVector tmp_beta = set_beta;
    eigenVector tmp_btse = set_se;
    vector<string> tmp_keep = snp_kept;

    //Remove colinearity
    MatrixXf D = C;
    int pos;
    int count = 0;
    vector<int> new_C_indx;
    vector<int> removed_snp;
    do {
        tmp_beta = snp_beta;
        tmp_btse = snp_btse;
        tmp_keep = snp_keep;

        new_C_indx.resize(0);
        pos = sbat_VIF_iter_rm_colin(D);
        /*
        if (pos > -1) {
            cout << "Remove colinearity from " << C.col(0).size() << " snps" <<  endl;
        } 
        */
        //Build new index
        for (i = 0 ; i < C.col(0).size() ; i++) {
            if (pos == i) continue;
            new_C_indx.push_back(i);
        }

        //Resize is destructive
        D.resize(new_C_indx.size(),new_C_indx.size()); 
        snp_beta.resize(new_C_indx.size());
        snp_btse.resize(new_C_indx.size());
        snp_keep.resize(new_C_indx.size());

        //Rebuild & assign new correlation matrix & beta, se, etc
        for (i = 0 ; i < new_C_indx.size() ; i++) {
           for (j = 0 ; j < new_C_indx.size() ; j++) {
               D(i,j) = C(new_C_indx[i],new_C_indx[j]);
           }
           snp_beta[i] = tmp_beta[new_C_indx[i]];
           snp_btse[i] = tmp_btse[new_C_indx[i]];
           snp_keep[i] = tmp_keep[new_C_indx[i]];
        }

        //Eigen resizes the matrix on the left-hand side automatically so that it matches the size of the matrix on the right-hand size
        C = D;
        count++;
    } while (pos > -1);

    cout << "index size " <<  new_C_indx.size() << endl;

    
    /* DEBUG
    string rgoodsnpfile = _out + ".rsnps";
    ofstream rogoodsnp(rgoodsnpfile.c_str());
    //ogoodsnp << "SNP" << endl;
    rogoodsnp << "snp beta se" << endl;
    for (i = 0; i < new_C_indx.size(); i++) rogoodsnp << snp_keep[i] << " "  << snp_beta[i] << " " << snp_btse[i] << endl;
    rogoodsnp.close();
    */
   

    /* 
    cout << " C matrix " << endl << C << endl;
    
    cout << " D matrix " << endl << D << endl;
    cout << " B  vector " << endl << snp_beta << endl;
    cout << " SE vector " << endl << snp_btse << endl;
    
    cout << " New Index " << endl;
    for (int abc = 0 ; abc < new_C_indx.size() ; abc++) cout << new_C_indx[abc] << " ";
    cout << endl;
    */

    snp_count = new_C_indx.size();

    /* DEBUG
    cout << "Number of snps retained: " << new_C_indx.size() << endl;
    cout << "Number of snps beta array: " << snp_beta.size() << endl;
    for (int aa=0 ; aa<new_C_indx.size() ; aa++) cout << " . " << new_C_indx[aa];
    */

    SE = snp_btse * snp_btse.transpose();
    V = SE.array() * D.cast<double>().array();
    //V.diagonal() = V.diagonal() * (1+0.000001);
    Vscore = snp_beta.transpose() * V.inverse() * snp_beta;
    Vscore_p = StatFunc::pchisq(Vscore, snp_beta.size());
    cout << "out " << Vscore << " " << Vscore_p << endl;
    
}

void gcta::sbat_multi(string sAssoc_file, string snpset_file)
{
    int i = 0, j = 0, ii=0;
    double Vscore = 0;
    double Vscore_p = 0;
    int snp_count = 0;
    //vector<int> num_snp_tested;

    // read SNP set file
    vector<string> set_name;
    vector< vector<string> > snpset;
    sbat_read_snpset(snpset_file, set_name, snpset);
    int set_num = set_name.size();

    // read SNP 'multi association results (including se & BETA/or)
    vector<string> snp_name;
    vector<int> snp_chr, snp_bp;
    vector<double> snp_pval;
    vector<double> snp_beta; // BETA - can be converted to log (code commented out futher down)
    vector<double> snp_btse; // se
    eigenVector set_beta;
    eigenVector set_se;
    cout << sAssoc_file << " file" << endl;
    sbat_multi_read_snpAssoc(sAssoc_file, snp_name, snp_chr, snp_bp, snp_pval, snp_beta, snp_btse);
    vector<double> snp_chisq(snp_pval.size());
    
    if (_mu.empty()) calcu_mu();
    cout << "\nRunning set-based multivariate association test (SBAT-MULTI)..." << endl;
    vector<double> set_pval(set_num), chisq_o(set_num);
    vector<int> snp_num_in_set(set_num);
    map<string, int>::iterator iter;
    map<string, int> snp_name_map;
    vector<int> num_snp_tested(set_num);
    for (i = 0; i < snp_name.size(); i++) snp_name_map.insert(pair<string,int>(snp_name[i], i));
    cout << "snps inserted" << endl;
    for (i = 0; i < set_num; i++) {
        bool skip = false;
        if(snpset[i].size() < 1) skip = true;
        vector<int> snp_indx;
        for(j = 0; j < snpset[i].size(); j++){
            iter = snp_name_map.find(snpset[i][j]);
            if(iter!=snp_name_map.end()) snp_indx.push_back(iter->second);
        }
        snp_num_in_set[i] = snp_indx.size();
        if(!skip && snp_num_in_set[i] > 20000){
            cout<<"Warning: Too many SNPs in the set ["<<set_name[i]<<"]. Maximum limit is 20000. This gene is ignored in the analysis."<<endl;
            skip = true;
        }
        if(skip){
            set_pval[i] = 2.0;
            snp_num_in_set[i] = 0;
            num_snp_tested[i] = 0;
            continue;
        }
        chisq_o[i] = 0;

        cout << "another" << endl;

        if((i + 1) % 100 == 0 || (i + 1) == set_num) cout << i + 1 << " of " << set_num << " sets.\r";


        //set_beta.resize(snpset[i].size());
        //set_se.resize(snpset[i].size());
        set_beta.resize(snp_indx.size()); //better index?
        set_se.resize(snp_indx.size());


        for (ii = 0; ii < snp_indx.size(); ii++) //was snpset[i].size()
        {
            set_beta[ii] = snp_beta[snp_indx[ii]];
            set_se[ii] = snp_btse[snp_indx[ii]];
        }

        //convert from OR to BETA
        //for(int i2 = 0 ; i2 < set_beta.size() ; i2++) set_beta[i2] = log(set_beta[i2]);

        snp_count=0;
        cout << "multi_calcu_V" << endl;
        sbat_multi_calcu_V(snp_indx, set_beta, set_se, Vscore, Vscore_p, snp_count, snp_name);
        //if not invertible -> call normal sbat / return "cant calc"
        num_snp_tested[i] = snp_count;
        chisq_o[i] = Vscore;
        set_pval[i] = Vscore_p;

    }

    cout << "Currently assuming BETA not OR score" << endl;
    string filename = _out + ".mbat";
    cout << "\nSaving the results of the SBAT analyses to [" + filename + "] ..." << endl;
    ofstream ofile(filename.c_str());
    if (!ofile) throw ("Can not open the file [" + filename + "] to write.");
    ofile << "Set\tSet.SNPs\tSNPsTested\tChisq(Obs)\tPvalue" << endl;
    for (i = 0; i < set_num; i++) {
        if(set_pval[i]>1.5) continue;
        ofile << set_name[i] << "\t" << snp_num_in_set[i] << "\t" << num_snp_tested[i] << "\t" << chisq_o[i] << "\t" << set_pval[i] << endl;
    }
    ofile.close();
}

void gcta::sbat_multi_read_snpAssoc(string snpAssoc_file, vector<string> &snp_name, vector<int> &snp_chr, vector<int> &snp_bp, vector<double> &snp_pval, vector<double> &snp_beta, vector<double> &snp_btse)
{
    ifstream in_snpAssoc(snpAssoc_file.c_str());
    if (!in_snpAssoc) throw ("Error: can not open the file [" + snpAssoc_file + "] to read.");
    cout << "\nReading SNP association results from [" + snpAssoc_file + "]." << endl;
    string str_buf;
    string A1_buf, A2_buf;
    double beta_buf;
    vector<string> ref_A_buf, bad_A1, bad_A2, bad_refA;
    vector<string> vs_buf, bad_snp;
    vector<string> snplist;
    int i=0, count = 0;
    map<string, int>::iterator iter;
    while (getline(in_snpAssoc, str_buf)) { 
        if (StrFunc::split_string(str_buf, vs_buf) != 7) throw ("Error: in line \"" + str_buf + "\".");

        A1_buf = vs_buf[1];
        A2_buf = vs_buf[2];
        beta_buf = atof(vs_buf[4].c_str());

        iter = _snp_name_map.find(vs_buf[0]);
        i = iter->second;
        if (iter == _snp_name_map.end()) continue;

        if (A1_buf != _allele1[i] && A1_buf != _allele2[i]) {
            bad_snp.push_back(_snp_name[i]);
            bad_A1.push_back(_allele1[i]);
            bad_A2.push_back(_allele2[i]);
            bad_refA.push_back(A1_buf);
            continue;
        }
        //MAYBE NOT NEEDED?
        if (A2_buf != _allele1[i] && A2_buf != _allele2[i]) {
            //also ignore bad A2?? -- new?
            bad_snp.push_back(_snp_name[i]);
            bad_A1.push_back(_allele1[i]);
            bad_A2.push_back(_allele2[i]);
            bad_refA.push_back(A1_buf);
            continue;
        }
 


        //update reference Allele based on assoc data
        if (A1_buf == _allele1[iter->second]) {
            _ref_A[iter->second] = _allele1[iter->second];
            _other_A[iter->second] = _allele2[iter->second];
        }
        else if (A1_buf == _allele2[iter->second]) {
            _ref_A[iter->second] = _allele2[iter->second];
            _other_A[iter->second] = _allele1[iter->second];
        }
        ref_A_buf.push_back(A1_buf);
        //do i need to change _mu or anything else??
        
        snp_name.push_back(vs_buf[0]);
       // ignore bp for now
        snp_beta.push_back(beta_buf); 
        snp_btse.push_back(atof(vs_buf[5].c_str()));
        snp_pval.push_back(atof(vs_buf[6].c_str()));


        

    }
    in_snpAssoc.close();
    snp_name.erase(unique(snp_name.begin(), snp_name.end()), snp_name.end());

    snplist = snp_name;
    update_id_map_kp(snplist, _snp_name_map, _include);

    vector<int> indx(_include.size()); 
    map<string, int> id_map;
    for (i = 0; i < snplist.size(); i++) id_map.insert(pair<string, int>(snplist[i], i));

    cout << "Association p-values of " << snp_name.size() << " SNPs have been included." << endl;

    vector<string> snp_name_buf(snp_name);
    vector<double> snp_pval_buf(snp_pval);
    vector<double> snp_beta_buf(snp_beta);
    vector<double> snp_btse_buf(snp_btse);

    snp_name.clear();
    snp_pval.clear();
    snp_beta.clear();
    snp_btse.clear();
    snp_name.resize(_include.size());
    snp_pval.resize(_include.size());
    snp_beta.resize(_include.size());
    snp_btse.resize(_include.size());
    i = 0;
    map<string, int> snp_name_buf_map;
    for (i = 0; i < snp_name_buf.size(); i++) snp_name_buf_map.insert(pair<string,int>(snp_name_buf[i], i));

    cout << "SNP INFO DUMP" << endl;

    #pragma omp parallel for
    for (i = 0; i < _include.size(); i++) {
        map<string, int>::iterator iter = snp_name_buf_map.find(_snp_name[_include[i]]);
        snp_name[i] = snp_name_buf[iter->second];
        snp_pval[i] = snp_pval_buf[iter->second];
        snp_beta[i] = snp_beta_buf[iter->second];
        snp_btse[i] = snp_btse_buf[iter->second];

    }
    snp_chr.resize(_include.size());
    snp_bp.resize(_include.size());
    #pragma omp parallel for
    for (i = 0; i < _include.size(); i++) {
        snp_chr[i] = _chr[_include[i]];
        snp_bp[i] = _bp[_include[i]];
    }
    if (_include.size() < 1) throw ("Error: no SNP is included in the analysis.");
    else if (_chr[_include[0]] < 1) throw ("Error: chromosome information is missing.");
    else if (_bp[_include[0]] < 1) throw ("Error: bp information is missing.");

    cout << _include.size() << " include size " << endl;

}


void gcta::rm_cor_sbat(MatrixXf &R, double R_cutoff, int m, vector<int> &rm_ID1) {
    //Slightly modified version of rm_cor_indi from grm.cpp
    
    cout << "Removing correlated snps with a cutoff of " << R_cutoff << " ..." << endl;

    int i = 0, j = 0, i_buf = 0;
   // vector<int> rm_ID1, rm_ID2;
    vector<int> rm_ID2;

    //use this as bases for rm cor
    float aval = 0;
    #pragma omp parallel for private(j)
    for (i = 0; i < m; i++) {
        for (j = 0; j < i; j++) {
            aval = R(i,j);
            if (fabs(aval) > R_cutoff ) { 
            //if (R(i,j) > R_cutoff ) { 
                rm_ID1.push_back(i);
                rm_ID2.push_back(j);
            }
        }
    }

    // count the number of appearance of each "position" in the vector, which involves a few steps
    vector<int> rm_uni_ID(rm_ID1);
    rm_uni_ID.insert(rm_uni_ID.end(), rm_ID2.begin(), rm_ID2.end());
    stable_sort(rm_uni_ID.begin(), rm_uni_ID.end());
    rm_uni_ID.erase(unique(rm_uni_ID.begin(), rm_uni_ID.end()), rm_uni_ID.end());
    map<int, int> rm_uni_ID_count;
    for (i = 0; i < rm_uni_ID.size(); i++) {
        i_buf = count(rm_ID1.begin(), rm_ID1.end(), rm_uni_ID[i]) + count(rm_ID2.begin(), rm_ID2.end(), rm_uni_ID[i]);
        rm_uni_ID_count.insert(pair<int, int>(rm_uni_ID[i], i_buf));
    }

    // swapping
    map<int, int>::iterator iter1, iter2;
    for (i = 0; i < rm_ID1.size(); i++) {
        iter1 = rm_uni_ID_count.find(rm_ID1[i]);
        iter2 = rm_uni_ID_count.find(rm_ID2[i]);
        if (iter1->second < iter2->second) {
            i_buf = rm_ID1[i];
            rm_ID1[i] = rm_ID2[i];
            rm_ID2[i] = i_buf;
        }
    }


    stable_sort(rm_ID1.begin(), rm_ID1.end());
    rm_ID1.erase(unique(rm_ID1.begin(), rm_ID1.end()), rm_ID1.end());

    }

void gcta::sbat_multi_gene(string sAssoc_file, string gAnno_file, int wind)
{
    int i = 0, j = 0, ii=0;
    double Vscore = 0;
    double Vscore_p = 0;
    int snp_count = 0;

    // read SNP 'multi association results (including se & BETA/or)
    vector<string> snp_name;
    vector<int> snp_chr, snp_bp;
    vector<double> snp_pval;
    vector<double> snp_beta; // BETA - can be converted to log (code commented out futher down)
    vector<double> snp_btse; // se
    eigenVector set_beta;
    eigenVector set_se;
    cout << sAssoc_file << " file" << endl;
    sbat_multi_read_snpAssoc(sAssoc_file, snp_name, snp_chr, snp_bp, snp_pval, snp_beta, snp_btse);
    vector<double> snp_chisq(snp_pval.size());

    // get start and end of chr
    int snp_num = snp_name.size();
    map<int, string> chr_begin_snp, chr_end_snp;
    chr_begin_snp.insert(pair<int, string>(snp_chr[0], snp_name[0]));
    for (i = 1; i < snp_num; i++) {
        if (snp_chr[i] != snp_chr[i - 1]) {
            chr_begin_snp.insert(pair<int, string>(snp_chr[i], snp_name[i]));
            chr_end_snp.insert(pair<int, string>(snp_chr[i - 1], snp_name[i - 1]));
        }
    }
    chr_end_snp.insert(pair<int, string>(snp_chr[snp_num - 1], snp_name[snp_num - 1]));
    
    // read gene list
    vector<string> gene_name;
    vector<int> gene_chr, gene_bp1, gene_bp2;
    sbat_read_geneAnno(gAnno_file, gene_name, gene_chr, gene_bp1, gene_bp2);

    // map genes to SNPs
    cout << "Mapping the physical positions of genes to SNP data (gene boundaries: " << wind / 1000 << "Kb away from UTRs) ..." << endl;

    int gene_num = gene_name.size();
    vector<int> num_snp_tested(gene_num);
    vector<string> gene2snp_1(gene_num), gene2snp_2(gene_num);
    vector<locus_bp>::iterator iter;
    map<int, string>::iterator chr_iter;
    vector<locus_bp> snp_vec;
    for (i = 0; i < snp_num; i++) snp_vec.push_back(locus_bp(snp_name[i], snp_chr[i], snp_bp[i]));
    #pragma omp parallel for private(iter, chr_iter)
    for (i = 0; i < gene_num; i++) {
        iter = find_if(snp_vec.begin(), snp_vec.end(), locus_bp(gene_name[i], gene_chr[i], gene_bp1[i] - wind));
        if (iter != snp_vec.end()) gene2snp_1[i] = iter->locus_name;
        else gene2snp_1[i] = "NA";
    }
    #pragma omp parallel for private(iter, chr_iter)
    for (i = 0; i < gene_num; i++) {
        if (gene2snp_1[i] == "NA") {
            gene2snp_2[i] = "NA";
            continue;
        }
        iter = find_if(snp_vec.begin(), snp_vec.end(), locus_bp(gene_name[i], gene_chr[i], gene_bp2[i] + wind));
        if (iter != snp_vec.end()){
            if (iter->bp ==  gene_bp2[i] + wind) gene2snp_2[i] = iter->locus_name;
            else {
                if(iter!=snp_vec.begin()){
                    iter--;
                    gene2snp_2[i] = iter->locus_name;
                }
                else gene2snp_2[i] = "NA";
            }
        }
        else {
            chr_iter = chr_end_snp.find(gene_chr[i]);
            if (chr_iter == chr_end_snp.end()) gene2snp_2[i] = "NA";
            else gene2snp_2[i] = chr_iter->second;
        }
    }
    int mapped = 0;
    for (i = 0; i < gene_num; i++) {
        if (gene2snp_1[i] != "NA" && gene2snp_2[i] != "NA") mapped++;
    }
    if (mapped < 1) throw ("Error: no gene can be mapped to the SNP data. Please check the input data regarding chr and bp.");
    else cout << mapped << " genes have been mapped to SNP data." << endl;

    // run sbat multi gene-based test

    
    if (_mu.empty()) calcu_mu();
    cout << "\nRunning set-based association test (SBAT) for genes ..." << endl;
    vector<double> gene_pval(gene_num), chisq_o(gene_num);
    vector<int> snp_num_in_gene(gene_num);
    map<string, int>::iterator iter1, iter2;
    map<string, int> snp_name_map;
    for (i = 0; i < snp_name.size(); i++) snp_name_map.insert(pair<string,int>(snp_name[i], i));
    for (i = 0; i < gene_num; i++) {
        iter1 = snp_name_map.find(gene2snp_1[i]);
        iter2 = snp_name_map.find(gene2snp_2[i]);
        bool skip = false;
        if (iter1 == snp_name_map.end() || iter2 == snp_name_map.end() || iter1->second >= iter2->second) skip = true;
        snp_num_in_gene[i] = iter2->second - iter1->second + 1;
        if(!skip && snp_num_in_gene[i] > 10000){
            cout<<"Warning: Too many SNPs in the gene region ["<<gene_name[i]<<"]. Maximum limit is 10000. This gene is ignored in the analysis."<<endl;
            skip = true;  
        } 
        if(skip){
            gene_pval[i] = 2.0;
            snp_num_in_gene[i] = 0;
            continue;
        }

        cout << gene_name[i] << " <- gene name " << endl;
        cout << snp_num_in_gene[i] << " snp num in gene " << endl;

        vector<int> snp_indx;
        for (j = iter1->second; j <= iter2->second; j++) snp_indx.push_back(j);            
 
        vector<string> snp_kept(snp_num_in_gene[i]);
        set_beta.resize(snp_num_in_gene[i]);
        set_se.resize(snp_num_in_gene[i]);
        for (ii = 0; ii < snp_num_in_gene[i]; ii++)
        {
            snp_kept[ii] = snp_name[snp_indx[ii]];
            set_beta[ii] = snp_beta[snp_indx[ii]];
            set_se[ii] = snp_btse[snp_indx[ii]];
        }


        chisq_o[i] = 0;
        for (j = iter1->second; j <= iter2->second; j++) chisq_o[i] += snp_chisq[j];
        if(snp_num_in_gene[i] == 1) {
            gene_pval[i] = StatFunc::pchisq(chisq_o[i], 1.0); //may change this - normal sbat 
            num_snp_tested[i]=1;
        }
        else {
            //vector<int> snp_indx; 
            //for (j = iter1->second; j <= iter2->second; j++) snp_indx.push_back(j);            
            //VectorXd eigenval;

            //snp details
            //cout << "Kept snps" << endl;
            //for (int aa = 0 ; aa < snp_name.size() ; aa++) cout << snp_name[aa] << endl;
            
        //write good snps (kept) to file DEBUG
        //
        /*
        string ngoodsnpfile = _out + ".ngoodsnps";
        ofstream nogoodsnp(ngoodsnpfile.c_str());
        //ogoodsnp << "SNP" << endl;
        nogoodsnp << "before multi" << endl;
        nogoodsnp << "snp beta se" << endl;
        for (i = 0; i < set_beta.size(); i++) nogoodsnp << snp_kept[i] << " " << set_beta[i] << " " << set_se[i] << endl;
        nogoodsnp.close();
        */
 

            snp_count=0;
            sbat_multi_calcu_V(snp_indx, set_beta, set_se, Vscore, Vscore_p, snp_count, snp_kept);
            num_snp_tested[i]=snp_count;
            chisq_o[i] = Vscore;
            gene_pval[i] = Vscore_p;

        }

        if((i + 1) % 100 == 0 || (i + 1) == gene_num) cout << i + 1 << " of " << gene_num << " genes.\r";

    }
    
    string filename = _out + ".gene.mbat";
    cout << "\nSaving the results of the SBAT analyses to [" + filename + "] ..." << endl;
    ofstream ofile(filename.c_str());
    if (!ofile) throw ("Can not open the file [" + filename + "] to write.");
    ofile << "Gene\tChr\tStart\tEnd\tNo.SNPs\tSNPsTested\tSNP_start\tSNP_end\tChisq(Obs)\tPvalue" << endl;
    for (i = 0; i < gene_num; i++) {
        if(gene_pval[i]>1.5) continue;
        ofile << gene_name[i] << "\t" << gene_chr[i] << "\t" << gene_bp1[i] << "\t" << gene_bp2[i] << "\t";
        ofile << snp_num_in_gene[i] << "\t" << num_snp_tested[i] << "\t" << gene2snp_1[i] << "\t" << gene2snp_2[i] << "\t" << chisq_o[i] << "\t" << gene_pval[i] << endl;
        //else ofile << "0\tNA\tNA\tNA\tNA" << endl;
    }
    ofile.close();
    
}


